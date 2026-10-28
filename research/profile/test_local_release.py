"""Capture-free host replay of the production no-EDID release path.

Synthetic buffers are only hashed/read as data; no vendor code or GPU is used.
Profile/fixture headers are virtual compiler inputs, never installed locally.
"""
import argparse
import hashlib
import importlib.util
import itertools
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BOOT = ROOT / 'internal-boot/boot'
spec = importlib.util.spec_from_file_location('release_topology_data', Path(__file__).with_name('test_local_topology.py'))
data = importlib.util.module_from_spec(spec)
spec.loader.exec_module(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clang', default='clang')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    if out.exists():
        parser.error('Output must be a new directory')
    out.mkdir(parents=True)
    (out / 'platform-config.h').write_text('#define GU_INTERNAL_PARTITION_INITIALIZER {0}\n#define GU_WINDOWS_PARTITION_INITIALIZER {0}\n')
    def run(name, command):
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        (out / (name + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(name + ': ' + (result.stdout + result.stderr)[-2500:])
        return result.stdout.strip()
    flags = [args.clang, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-fshort-wchar',
             '-D_CRT_SECURE_NO_WARNINGS', '-DGU_PROFILE_LOCAL', '-DGU_HEADLESS_RESEARCH',
             '-DGU_SINGLE_RTX_CONSOLE', '-DGU_FULL_UNLOCK', '-DGU_AUTOMATIC_BOOT',
             '-I', str(out), '-I', str(ROOT / 'deps/edk2/MdePkg/Include'), '-I', str(ROOT / 'deps/edk2/MdePkg/Include/X64')]
    sources = [str(BOOT / 'test_headless_full.c'), str(BOOT / 'console-flow.c'),
               str(ROOT / 'internal-boot/unlock/checks.c'), str(ROOT / 'internal-boot/unlock/sha256.c')]
    summaries, child_checks = [], []
    for bus, children, command, memory in itertools.product((1, 0x2b), (1, 2, 3, 4), (2, 3), (0x6000, 0x5ff4)):
        name = f'bus{bus:x}-children{children}-cmd{command}-memory{memory:x}'
        case = out / name
        case.mkdir()
        capture = case / 'DIAG.TXT'
        capture.write_text(data.synthetic_capture(bus=bus, children=children, command=command, memory=memory,
                                                 child_adr=0x10 if bus == 1 else 0xa0), encoding='ascii')
        admitted = data.analyze(capture.read_text())
        defines = admitted['defines'] + [
            ('GU_NVGOP_TEXT_BYTES', '0x24426U'),
            ('GU_NVGOP_TEXT_SHA256', '"' + hashlib.sha256(bytes(0x24426)).hexdigest() + '"')]
        header = case / 'profile-local.h'
        header.write_text('/* Synthetic host-only inputs; not hardware evidence. */\n'
                          '#define GU_LOCAL_PROFILE_GENERATOR 1\n#define GU_PROFILE_NAME "LOCAL-SYNTHETIC-REPLAY"\n' +
                          '\n'.join(f'#define {key} {value}' for key, value in defines) + '\n')
        fixture = case / 'replay-fixture.h'
        run(name + '-fixture', [sys.executable, '-I', str(BOOT / 'make-console-fixture.py'), str(capture),
                              '--single-rtx', '--no-igpu', '--no-edid-driver', '--no-edid-child',
                              '--rtx-id=220410de', '--rtx-fb=0x000000fcc0000000', '--output=' + str(fixture)])
        config = data.configuration(command)
        import struct
        config_path = case / 'config.bin'
        config_path.write_bytes(struct.pack('<64I', *config))
        prom = case / 'prom.bin'; prom.write_bytes(bytes(1048576))
        nv = case / 'nvgop.bin'; nv.write_bytes(bytes(0x2be40))
        overlay = case / 'overlay.json'
        overlay.write_text(json.dumps({'version': 0, 'roots': [
            {'type': 'file', 'name': str(ROOT / 'internal-boot/unlock/profile-local.h'), 'external-contents': str(header), 'use-external-name': False},
            {'type': 'file', 'name': str(BOOT / 'build/r2/replay-fixture.h'), 'external-contents': str(fixture), 'use-external-name': False}]}))
        exe = case / ('release.exe' if sys.platform == 'win32' else 'release')
        run(name + '-compile', flags + ['-ivfsoverlay', str(overlay), *sources, '-o', str(exe)])
        summary = run(name, [str(exe), str(nv), str(prom), str(config_path)])
        summaries.append({'case': name, 'result': summary})
        print(name + ': ' + summary, flush=True)
        if children == 1 and command == 2 and memory == 0x6000:
            probe = case / 'probe-report.txt'
            # test_checks also tests a specific linked-capability layout. Supply that
            # layout synthetically, retaining this local profile's identity and bus.
            checks_config = config.copy()
            checks_config[13] = 0x60
            checks_config[16:] = [0] * 48
            for at,value in {0x60:0x00036801,0x68:0x00807805,0x78:0x0012b410,
                             0x7c:1<<28,0xb4:0x00140009,0xc8:0x00050011}.items():
                checks_config[at//4] = value
            probe.write_text(''.join(f'config_offset=0x{i*4:x}\nvalue=0x{value:08x}\n' for i,value in enumerate(checks_config)))
            for job, inputs, arguments in (
                ('checks', [str(ROOT / 'internal-boot/unlock/tests/test_checks.c'), str(ROOT / 'internal-boot/unlock/checks.c')], [str(probe)]),
                ('port', [str(ROOT / 'internal-boot/unlock/tests/test_port.c'), str(ROOT / 'internal-boot/unlock/port.c')], [])):
                binary = case / (job + ('.exe' if sys.platform == 'win32' else ''))
                run(name + '-' + job + '-compile', flags + ['-ivfsoverlay', str(overlay), '-DGU_HOST_WRITE_TEST', *inputs, '-o', str(binary)])
                result = run(name + '-' + job, [str(binary), *arguments])
                child_checks.append({'case': name, 'job': job, 'result': result})
                print(name + ': ' + result, flush=True)
    (out / 'summary.json').write_text(json.dumps({'synthetic_only': True, 'hardware_access': False,
                                                 'cases': summaries, 'child_checks': child_checks}, indent=2))


if __name__ == '__main__':
    main()
