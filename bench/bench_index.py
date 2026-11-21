#!/usr/bin/env python3
"""Benchmark parallel inverted index build time vs thread count (NFR-P1)."""

import argparse
import csv
import ctypes
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile
import time

os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "cse_matplotlib"))

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import requests


def collect_hardware_info() -> dict:
    info = {
        "platform": platform.platform(),
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "logical_cores": os.cpu_count(),
        "python_version": platform.python_version(),
    }

    if platform.system() == "Windows":
        try:
            import winreg

            with winreg.OpenKey(
                winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0"
            ) as key:
                info["cpu_name"] = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
        except Exception:
            pass

        try:
            class MEMORYSTATUSEX(ctypes.Structure):
                _fields_ = [
                    ("dwLength", ctypes.c_ulong),
                    ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
                ]

            mem_status = MEMORYSTATUSEX(dwLength=ctypes.sizeof(MEMORYSTATUSEX))
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(mem_status)):
                info["total_ram_gb"] = round(mem_status.ullTotalPhys / (1024**3), 2)
        except Exception:
            pass

        if "cpu_name" not in info or "total_ram_gb" not in info:
            try:
                ps_cmd = (
                    "Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name; "
                    "[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 2)"
                )
                out = subprocess.check_output(["powershell", "-NoProfile", "-Command", ps_cmd], text=True, timeout=5)
                lines = [line.strip() for line in out.splitlines() if line.strip()]
                if len(lines) >= 2:
                    info.setdefault("cpu_name", lines[0])
                    info.setdefault("total_ram_gb", float(lines[1]))
            except Exception:
                pass
    elif platform.system() == "Linux":
        try:
            meminfo = Path("/proc/meminfo").read_text(encoding="utf-8")
            for line in meminfo.splitlines():
                if line.startswith("MemTotal:"):
                    kb = int(line.split()[1])
                    info["total_ram_gb"] = round(kb / (1024 * 1024), 2)
                    break
        except Exception:
            pass

    return info


def main() -> int:
    parser = argparse.ArgumentParser(description="Index build scalability benchmark")
    parser.add_argument("--host", default="http://localhost:8080", help="Server base URL")
    parser.add_argument(
        "--threads",
        nargs="+",
        type=int,
        default=[1, 2, 4, 8, 16, 32, 64, 128],
        help="Thread counts to benchmark",
    )
    parser.add_argument("--runs", type=int, default=5, help="Number of runs per thread count (default: 5)")
    parser.add_argument("--output-dir", default="bench/results", help="Results directory")
    args = parser.parse_args()

    base_url = args.host.rstrip("/")
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    try:
        health = requests.get(f"{base_url}/api/health", timeout=5)
        health.raise_for_status()
    except Exception as exc:
        print(f"Server health check failed at {base_url}: {exc}", file=sys.stderr)
        return 1

    hw_info = collect_hardware_info()
    (out_dir / "hardware_info.json").write_text(json.dumps(hw_info, indent=2), encoding="utf-8")
    print(f"Hardware: {json.dumps(hw_info)}")

    rows = []
    summaries = []
    session = requests.Session()

    for thread_count in sorted(args.threads):
        run_times = []
        docs_count = 0
        print(f"\nBenchmarking threads={thread_count} ({args.runs} runs)...")

        for run_idx in range(1, args.runs + 1):
            t0 = time.perf_counter()
            resp = session.post(
                f"{base_url}/api/index",
                json={"threads": thread_count},
                headers={"Connection": "close"},
                timeout=600,
            )
            client_ms = (time.perf_counter() - t0) * 1000.0
            if resp.status_code != 200:
                print(f"Indexing failed (HTTP {resp.status_code}): {resp.text}", file=sys.stderr)
                return 1
            data = resp.json()
            server_ms = int(data["time_ms"])
            docs_count = int(data["documents_indexed"])
            run_times.append(server_ms)
            rows.append(
                {
                    "threads": thread_count,
                    "run": run_idx,
                    "documents_indexed": docs_count,
                    "server_time_ms": server_ms,
                    "client_time_ms": round(client_ms, 2),
                }
            )
            print(f"  run {run_idx}: {server_ms} ms ({docs_count} docs)")

        median_ms = statistics.median(run_times)
        summaries.append(
            {
                "threads": thread_count,
                "documents": docs_count,
                "median_ms": median_ms,
                "min_ms": min(run_times),
                "max_ms": max(run_times),
            }
        )

    baseline_ms = summaries[0]["median_ms"]
    baseline_threads = summaries[0]["threads"]
    for item in summaries:
        speedup = baseline_ms / item["median_ms"] if item["median_ms"] > 0 else 0.0
        relative_threads = item["threads"] / baseline_threads
        efficiency = speedup / relative_threads if relative_threads > 0 else 0.0
        item["speedup"] = round(speedup, 3)
        item["efficiency"] = round(efficiency, 3)

    csv_path = out_dir / "index_benchmark.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f, fieldnames=["threads", "run", "documents_indexed", "server_time_ms", "client_time_ms"]
        )
        writer.writeheader()
        writer.writerows(rows)

    summary_csv_path = out_dir / "index_summary.csv"
    with summary_csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f, fieldnames=["threads", "documents", "median_ms", "min_ms", "max_ms", "speedup", "efficiency"]
        )
        writer.writeheader()
        writer.writerows(summaries)

    thread_vals = [s["threads"] for s in summaries]
    median_sec = [s["median_ms"] / 1000.0 for s in summaries]
    err_low = [(s["median_ms"] - s["min_ms"]) / 1000.0 for s in summaries]
    err_high = [(s["max_ms"] - s["median_ms"]) / 1000.0 for s in summaries]
    speedups = [s["speedup"] for s in summaries]
    efficiencies = [s["efficiency"] * 100.0 for s in summaries]

    fig, ax = plt.subplots(figsize=(8.5, 5))
    ax.errorbar(thread_vals, median_sec, yerr=[err_low, err_high], marker="o", linewidth=2, capsize=4)
    ax.set_title(f"Inverted Index Build Time vs Thread Count (Median of {args.runs} Runs)")
    ax.set_xlabel("Thread Count (N)")
    ax.set_ylabel("Build Time (seconds)")
    ax.set_xscale("log", base=2)
    ax.set_xticks(thread_vals)
    ax.set_xticklabels([str(t) for t in thread_vals])
    ax.grid(True, linestyle="--", alpha=0.5)
    fig.tight_layout()
    fig.savefig(out_dir / "index_build_time.png", dpi=150)
    plt.close(fig)

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11.5, 4.6))
    ax1.plot(thread_vals, speedups, marker="o", linewidth=2, label="Measured S(N)")
    ax1.plot(thread_vals, thread_vals, linestyle="--", color="gray", label="Ideal Linear S(N) = N")
    ax1.set_title("Speedup S(N) = T(1) / T(N)")
    ax1.set_xlabel("Thread Count (N)")
    ax1.set_ylabel("Speedup")
    ax1.set_xscale("log", base=2)
    ax1.set_xticks(thread_vals)
    ax1.set_xticklabels([str(t) for t in thread_vals])
    ax1.set_ylim(0, max(max(speedups) * 1.35 + 1.0, 2.0))
    ax1.legend()
    ax1.grid(True, linestyle="--", alpha=0.5)

    ax2.plot(thread_vals, efficiencies, marker="s", color="#1a7f37", linewidth=2)
    ax2.set_title("Parallel Efficiency E(N) = S(N) / N")
    ax2.set_xlabel("Thread Count (N)")
    ax2.set_ylabel("Efficiency (%)")
    ax2.set_xscale("log", base=2)
    ax2.set_xticks(thread_vals)
    ax2.set_xticklabels([str(t) for t in thread_vals])
    ax2.set_ylim(0, 115)
    ax2.grid(True, linestyle="--", alpha=0.5)

    fig.tight_layout()
    fig.savefig(out_dir / "index_speedup_efficiency.png", dpi=150)
    plt.close(fig)

    print("\nSummary:")
    for item in summaries:
        print(
            f"  threads={item['threads']:>2}: median={item['median_ms']:.1f} ms | "
            f"S={item['speedup']:.2f}x | E={item['efficiency'] * 100:.1f}%"
        )
    print(f"\nSaved artifacts to {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
