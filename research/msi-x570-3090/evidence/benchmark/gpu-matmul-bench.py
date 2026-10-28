# /// script
# requires-python = ">=3.12"
# dependencies = ["torch", "numpy"]
# [tool.uv.sources]
# torch = { index = "pytorch-cu128" }
# [[tool.uv.index]]
# name = "pytorch-cu128"
# url = "https://download.pytorch.org/whl/cu128"
# explicit = true
# ///
"""Tensor-core throughput before/after the RTX 3090 unlock (TASK-22).

Method of the upstream B550M evidence (evidence/b550m-3090-usb/summary.json): torch.matmul,
FP16/BF16 with FP32 accumulation and reduced-precision reduction off, FP32 with TF32 off as the
control, TF32 on; median of N runs of R matmuls; TFLOPS. Power draw, SM clock and throttle reasons
are sampled after each mode (n=8192 can hit the power limit once unlocked).
Records driver, power limit, clocks, temperature and other GPU processes so a before and an
after run can be compared under the same conditions.

    uv run scripts/gpu-matmul-bench.py --label locked --out docs/evidence/rtx3090-unlock
"""
import argparse, datetime, json, statistics, subprocess, time
from pathlib import Path

import torch


def smi(query):
    out = subprocess.run(['nvidia-smi', f'--query-gpu={query}', '--format=csv,noheader,nounits'],
                         capture_output=True, text=True, check=True).stdout.strip()
    return dict(zip(query.split(','), (v.strip() for v in out.split(','))))


def gpu_processes():
    out = subprocess.run(['nvidia-smi', '--query-compute-apps=pid,process_name', '--format=csv,noheader'],
                         capture_output=True, text=True).stdout
    return sorted({line.split(',')[1].strip().split('\\')[-1] for line in out.splitlines() if ',' in line})


def bench(n, dtype, tf32, runs, reps):
    torch.backends.cuda.matmul.allow_tf32 = tf32
    a = torch.randn(n, n, device='cuda', dtype=dtype)
    b = torch.randn(n, n, device='cuda', dtype=dtype)
    for _ in range(5):
        torch.matmul(a, b)
    torch.cuda.synchronize()
    rates = []
    for _ in range(runs):
        t = time.perf_counter()
        for _ in range(reps):
            torch.matmul(a, b)
        torch.cuda.synchronize()
        rates.append(2 * n ** 3 * reps / (time.perf_counter() - t) / 1e12)
    return {'median_tflops': round(statistics.median(rates), 2), 'runs_tflops': [round(r, 2) for r in rates]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--label', required=True, help='locked or unlocked, plus anything that tells runs apart')
    ap.add_argument('--out', required=True)
    ap.add_argument('--sizes', default='4096,8192')
    ap.add_argument('--runs', type=int, default=5)
    ap.add_argument('--reps', type=int, default=100)
    a = ap.parse_args()
    torch.backends.cudnn.allow_tf32 = False
    # torch defaults both to True; upstream measured with them off (FP32 accumulation throughout).
    torch.backends.cuda.matmul.allow_fp16_reduced_precision_reduction = False
    torch.backends.cuda.matmul.allow_bf16_reduced_precision_reduction = False
    modes = [('fp32', torch.float32, False), ('tf32', torch.float32, True),
             ('fp16', torch.float16, False), ('bf16', torch.bfloat16, False)]
    q = 'name,pci.bus_id,driver_version,vbios_version,power.limit,clocks.max.sm,clocks.max.mem,temperature.gpu,utilization.gpu'
    result = {'label': a.label, 'started': datetime.datetime.now().astimezone().isoformat(timespec='seconds'),
              'torch': torch.__version__, 'cuda': torch.version.cuda, 'gpu_before': smi(q),
              'other_gpu_processes': gpu_processes(),
              'reduced_precision_reduction': {'fp16': False, 'bf16': False}, 'method': f'median of {a.runs} runs of {a.reps} matmuls',
              'results': {}}
    for n in (int(s) for s in a.sizes.split(',')):
        for name, dtype, tf32 in modes:
            result['results'][f'{n}/{name}'] = bench(n, dtype, tf32, a.runs, a.reps)
            result['results'][f'{n}/{name}']['after'] = smi(
                'clocks.sm,power.draw,temperature.gpu,clocks_throttle_reasons.active')
            print(f'{n:5d} {name}: {result["results"][f"{n}/{name}"]["median_tflops"]:7.2f} TFLOPS', flush=True)
    result['gpu_after'] = smi(q)
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M')
    path = out / f'bench-{a.label}-{stamp}ET.json'
    path.write_text(json.dumps(result, indent=2) + '\n')
    print(path)


if __name__ == '__main__':
    main()
