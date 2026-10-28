"""Turn a real successful diagnostic into an ignored host replay fixture.

No firmware or GPU access. Actual addresses are replaced by ordinal handles.
The source log stays private/local; it is not copied into tracked source.
"""
import hashlib
import importlib.util
import re
import sys
import uuid
from pathlib import Path

source = Path(sys.argv[1])
single = '--single-rtx' in sys.argv[2:]
def option(name, default):
    """--name=value override; defaults are the original B650M/3090 FE capture."""
    for arg in sys.argv[2:]:
        if arg.startswith(f'--{name}='):
            return arg.split('=', 1)[1]
    return default
rtx_id = option('rtx-id', '220410de')
amd_id = option('amd-id', '164e1002')
rtx_fb = option('rtx-fb', '0x000000fcc0000000')
raw = source.read_bytes()
text = raw.decode().replace('\r\n', '\n')
graph_spec = importlib.util.spec_from_file_location('fixture_capture_graph', Path(__file__).resolve().parents[2] / 'research/profile/capture_graph.py')
graph = importlib.util.module_from_spec(graph_spec)
graph_spec.loader.exec_module(graph)
parsed = graph.parse_handles(text)
blocks = {hex(h): block['raw'] for h, block in parsed.items()}
indices = {h: i + 1 for i, h in enumerate(blocks)}
edges = []
for handle, block in parsed.items():
    for protocol, agent, controller, attrs, count in block['edges']:
        if attrs not in (1, 2, 4):
            edges.append((indices[hex(handle)], indices[hex(agent)], indices[hex(controller)] if controller else 0, uuid.UUID(protocol), attrs, count))
def one(predicate):
    values = [h for h, block in blocks.items() if predicate(block)]
    if len(values) != 1:
        raise ValueError('Expected exactly one role')
    return indices[values[0]]
no_igpu = '--no-igpu' in sys.argv[2:]
if no_igpu and not single:
    raise ValueError('--no-igpu is only for the single-RTX console replay')
if no_igpu:
    for block in blocks.values():
        config = re.search(r'^pci config-dwords=([^\n]+)$', block, re.M)
        if config:
            words = [int(word, 16) for word in config[1].split()]
            if words[0] & 0xffff != 0x10de and words[2] >> 24 == 3:
                raise ValueError('--no-igpu disagrees with the display inventory')
# This firmware binds no EDID/mouse driver to the RTX display handles (GU_NO_EDID_DRIVER).
no_edid_driver = '--no-edid-driver' in sys.argv[2:]
if no_edid_driver and not single:
    raise ValueError('--no-edid-driver is only for the single-RTX console replay')
EDID_DISCOVERED = '1c0c34f6-d380-41fa-a049-8ad06c1a66aa'
if no_edid_driver and any(str(e[3]) == EDID_DISCOVERED for e in edges):
    raise ValueError('--no-edid-driver was given but the graph has an active EdidDiscovered owner')
roles = {
    'RtxPci': one(lambda b: f'pci config-dwords={rtx_id}' in b),
    # Without an integrated GPU the replay has no second adapter. The legacy two-adapter test
    # code that test_r2.c includes still names this handle, so give it an ordinal that exists in
    # no graph block; the single-RTX scenarios never reach it.
    'AmdPci': len(blocks) + 1 if no_igpu else one(lambda b: f'pci config-dwords={amd_id}' in b),
    'Rtx': one(lambda b: f'gop framebuffer={rtx_fb}' in b),
    'Amd': 0 if single else one(lambda b: 'gop framebuffer=0x000000fce0000000' in b),
}
if single:
    child_records = [e for e in edges if e[0] == roles['RtxPci'] and str(e[3]) == '09576e91-6d3f-11d2-8e39-00a0c969723b' and e[4] == 8 and e[2] != roles['Rtx']]
    # The B650M capture had a second EDID-only RTX child; a one-monitor
    # capture has none. Without an EDID driver the headless release takes up to
    # three more (four children); with one, more than one is still unreviewed.
    extra_limit = 3 if no_edid_driver else 1
    if len(child_records) > extra_limit or ('--no-edid-child' not in sys.argv[2:] and len(child_records) != 1):
        raise ValueError('Expected at most three additional RTX display children' if no_edid_driver else
                         'Expected one additional RTX EDID-only child')
    if child_records:
        roles['EdidChild'] = child_records[0][2]
    else:
        roles['EdidChild'] = 0  # No physical second child; tests must not invent one.
for role, protocol, controller in [
    ('OutAgent', '387477c2-69c7-11d2-8e39-00a0c969723b', roles['Rtx']),
    ('GfxAgent', '9042a9de-23dc-4a38-96fb-7aded080516a', roles['Rtx']),
    ('NvAgent', '4cf5b200-68b8-4ca5-9eec-b23e3f50029a', roles['RtxPci']),
    ('EdidAgent', EDID_DISCOVERED, roles['Rtx']),
]:
    if no_edid_driver and role == 'EdidAgent':
        continue
    records = [e for e in edges if e[0] == controller and str(e[3]) == protocol and e[4] == 16]
    if len(records) != 1:
        raise ValueError('Expected a unique consumer')
    roles[role] = records[0][1]
    block = blocks[next(h for h, i in indices.items() if i == roles[role])]
    roles[role.removesuffix('Agent') + 'Image'] = indices[hex(int(re.search(r'driver image=(0x[0-9a-f]+)', block)[1], 16))]
console = re.search(r'console out=(0x[0-9a-f]+) error=(0x[0-9a-f]+) input=(0x[0-9a-f]+)', text)
for role, value in zip(['Console', 'Error', 'Input'], console.groups()):
    roles[role] = indices[hex(int(value, 16))]
lines = [f'/* Local diagnostic sha256 {hashlib.sha256(raw).hexdigest()} */', f'#define FX_HANDLES {len(blocks)}',
         f'#define FX_NO_EDID_DRIVER {1 if no_edid_driver else 0}',
         f'#define FX_EXTRA_DISPLAY_CHILD {len(child_records) if single else 0}']
if roles.get('EdidChild'):
    child_block = blocks[next(h for h, index in indices.items() if index == roles['EdidChild'])]
    path_rows = re.findall(r'^path-node raw=([0-9a-f]+)$', child_block, re.M)
    child_path = b''.join(bytes.fromhex(row) for row in path_rows)
    if len(child_path) != 36:
        raise ValueError('Additional display child requires its recorded 36-byte path')
    lines.append('#define FX_EDID_CHILD_PATH_BYTES ' + ','.join(str(byte) for byte in child_path))
if single and len(child_records) > 1:
    paths = []
    for record in child_records:
        rows = re.findall(r'^path-node raw=([0-9a-f]+)$', blocks[next(h for h, index in indices.items() if index == record[2])], re.M)
        path = b''.join(bytes.fromhex(row) for row in rows)
        if len(path) != 36:
            raise ValueError('Additional display child requires its recorded 36-byte path')
        paths.append('{' + ','.join(str(byte) for byte in path) + '}')
    lines.append('#define FX_EXTRA_CHILD_HANDLES ' + ','.join(f'(EFI_HANDLE){record[2]}' for record in child_records))
    lines.append('#define FX_EXTRA_CHILD_PATHS ' + ','.join(paths))
for role, value in roles.items():
    lines.append(f'#define FX_{role} ((EFI_HANDLE){value})')
lines.append('static const EDGE FixtureEdges[]={')
for h, a, c, g, attrs, count in edges:
    data = g.bytes
    guid = f'{{0x{g.time_low:x},0x{g.time_mid:x},0x{g.time_hi_version:x},{{' + ','.join(f'0x{x:x}' for x in data[8:]) + '}}'
    lines.append(f'{{(EFI_HANDLE){h},(EFI_HANDLE){a},(EFI_HANDLE){c},{guid},{attrs},{count}}},')
lines.append('};')
for role in ['Rtx', 'Amd']:
    if role == 'Amd' and no_igpu:
        lines.append('static const UINT32 FixtureAmdConfig[16]={0};')
        continue
    block = blocks[next(h for h, i in indices.items() if i == roles[role + 'Pci'])]
    cfg = re.search(r'pci config-dwords=([^\n]+)', block)[1].split()
    if len(cfg) != 16:
        raise ValueError('Incomplete PCI snapshot')
    lines.append('static const UINT32 Fixture' + role + 'Config[16]={' + ','.join('0x' + x for x in cfg) + '};')
default_target = Path(__file__).parent / ('build/r2/replay-fixture.h' if single else 'build/console/replay-fixture.h')
target = Path(option('output', str(default_target)))
target.parent.mkdir(parents=True, exist_ok=True)
target.write_text('\n'.join(lines) + '\n')
print(f'Local replay fixture: {len(blocks)} handles, {len(edges)} active ownership records; ordinal addresses only')
