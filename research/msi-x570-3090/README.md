# MSI X570 / Gigabyte RTX 3090: I/O-decode pre-step and evidence

Machine: MSI MPG X570 GAMING PRO CARBON WIFI (MS-7B93), AMI BIOS 1.Q2, Ryzen 9 3950X (no iGPU),
Gigabyte RTX 3090 `10de:2204` / `1458:403a`, VBIOS 94.02.59.00.5e, NVIDIA GOP 0x6000a, at `0000:2d:00.0`.
Windows 11 Pro 26300, driver 621.01, Resizable BAR on, Secure Boot off.

Base: `20c44fe`. The commits on this branch are:

1. the local diff that was tested and run (generator display-child limit 1-4, replay tests, simulation
   PciIo record derived from the captured command register, and the local `proven.json` 0x6000a entry);
2. this folder.

In `proven.json`, only the wording of the 0x6000a `evidence` string differs from the tree that was tested.

## The problem

Both read-only captures had PCI command `0x0002` (I/O off, memory on), but a PciIo attribute record of
`0x300` (I/O + memory). On this AMI firmware, `PciIo->Attributes(Enable, IO)` returns success without
writing the hardware bit when the record already holds IO. The NVIDIA GOP ignores that status.

## The pre-step (`prestep/`)

`prestep.c` + `decode-sync.c` build a small EFI that runs before the project's headless wrapper. The
wrapper is unmodified and is staged as `\EFI\BOOT\NEXT.EFI` on the same stick. The pre-step:

1. logs the GPU's and bridge's config space, flushing the log before any write;
2. calls `Attributes(Disable, IO)` (`Enable, IO` is tried first and observed to change nothing);
3. requires no other config-space change and stable rereads at 1, 2, 4 and 8 s;
4. then starts `NEXT.EFI`.

With `PRESTEP_GATE=1`, any failure returns `EFI_NOT_STARTED` before the display is released, so the
firmware falls through to Windows Boot Manager.

- `build.ps1`: clang/lld-link 20.1.8 with the project's EDK2 headers. It reproduces the hashes in the
  `*.sha256` files.
- `test.sh`: gcc host tests in WSL, run with the mock UEFI in `test_prestep.c` and `test_decode_sync.c`.
- `mutate.sh`: a mutation check; each guard broken must fail the tests.
- `stage.ps1`: puts the pre-step in front of an image on the stick.

These scripts carry this machine's paths (the project clone at `C:\temp\gpu-unlock-3090`); adjust
them for your own.

## Evidence (`evidence/`)

- `prestep-probe-boots/PREST00.TXT`, `PREST01.TXT`: two probe boots. Both show
  `0x0002/0x300` → `0x0002/0x200`, stable.
- `unlock-boot/`: the unlock boot's pre-step log, wrapper log (`HLS000000.TXT`) and child report.
  The report ends `transaction result=0`, `FINAL status=0 ... clean=1`. `SHA256SUMS` covers these files. `HLS000000.TXT` has the NVMe ID and partition GUIDs redacted; see `REDACTED.md`.
- `benchmark/`: `gpu-matmul-bench.py` and three locked and three unlocked runs (JSON), plus a plot.
  The method is torch.matmul with FP16/BF16 on FP32 accumulation, reduced-precision reduction off, TF32,
  and FP32 as the control. Each run is the median of 5 × 100 matmuls.

No firmware, PROM, VBIOS or GOP image dumps are included.

## Boots

Every probe and unlock boot started from a Windows restart into the firmware boot menu. On this
board a restart resets the GPU to its power-on state; the cold-state registers read the power-on
values each time.
