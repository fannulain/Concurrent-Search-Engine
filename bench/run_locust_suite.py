#!/usr/bin/env python3
"""Run Locust sustained load and spike tests and generate summary plots."""

import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "cse_matplotlib"))

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import requests


def parse_float(raw: str, default: float = 0.0) -> float:
    if not raw or raw == "N/A":
        return default
    try:
        return float(raw)
    except ValueError:
        return default


def parse_int(raw: str, default: int = 0) -> int:
    if not raw or raw == "N/A":
        return default
    try:
        return int(float(raw))
    except ValueError:
        return default


def ensure_index_ready(base_url: str, index_threads: int) -> None:
    stats = requests.get(f"{base_url}/api/stats", timeout=5).json()
    if int(stats.get("documents", 0)) == 0:
        print(f"Index is empty. Building index with {index_threads} threads...")
        resp = requests.post(f"{base_url}/api/index", json={"threads": index_threads}, timeout=600)
        resp.raise_for_status()
        print(f"Index built: {resp.json()}")


def load_stats_summary(stats_csv: Path) -> dict:
    endpoints = []
    aggregated = None
    with stats_csv.open("r", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            entry = {
                "method": row.get("Type", ""),
                "name": row.get("Name", ""),
                "requests": parse_int(row.get("Request Count", "0")),
                "failures": parse_int(row.get("Failure Count", "0")),
                "median_ms": round(parse_float(row.get("Median Response Time", "0")), 2),
                "avg_ms": round(parse_float(row.get("Average Response Time", "0")), 2),
                "p95_ms": round(parse_float(row.get("95%", "0")), 2),
                "p99_ms": round(parse_float(row.get("99%", "0")), 2),
                "max_ms": round(parse_float(row.get("Max Response Time", "0")), 2),
                "rps": round(parse_float(row.get("Requests/s", "0")), 2),
            }
            if entry["name"] == "Aggregated":
                aggregated = entry
            else:
                endpoints.append(entry)

    return {"aggregated": aggregated, "endpoints": endpoints}


def load_history_series(history_csv: Path) -> dict:
    times: list[float] = []
    users: list[int] = []
    rps: list[float] = []
    fail_rps: list[float] = []
    p50: list[float] = []
    p95: list[float] = []

    t0: float | None = None
    with history_csv.open("r", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if row.get("Name") != "Aggregated":
                continue
            ts = parse_float(row.get("Timestamp", "0"))
            if t0 is None:
                t0 = ts
            times.append(ts - t0)
            users.append(parse_int(row.get("User Count", "0")))
            rps.append(parse_float(row.get("Requests/s", "0")))
            fail_rps.append(parse_float(row.get("Failures/s", "0")))
            p50.append(parse_float(row.get("50%", "0")))
            p95.append(parse_float(row.get("95%", "0")))

    return {
        "elapsed_s": times,
        "users": users,
        "rps": rps,
        "fail_rps": fail_rps,
        "p50_ms": p50,
        "p95_ms": p95,
    }


def plot_profile(profile: str, history: dict, out_path: Path) -> None:
    t = history["elapsed_s"]
    title_label = "Sustained Load Test" if profile == "load" else "Traffic Spike & Recovery Test"

    fig, (ax_top, ax_bot) = plt.subplots(2, 1, figsize=(10, 7), sharex=True)

    line_rps = ax_top.plot(t, history["rps"], color="#0969da", linewidth=2, label="Throughput (req/s)")
    ax_top.set_ylabel("Throughput (req/s)", color="#0969da")
    ax_top.tick_params(axis="y", labelcolor="#0969da")
    ax_top.grid(True, linestyle="--", alpha=0.5)

    ax_users = ax_top.twinx()
    line_users = ax_users.step(t, history["users"], where="post", color="#8250df", linewidth=1.8, linestyle="--", label="Active Users")
    ax_users.set_ylabel("Concurrent Users", color="#8250df")
    ax_users.tick_params(axis="y", labelcolor="#8250df")

    lines = line_rps + line_users
    labels = [line.get_label() for line in lines]
    ax_top.legend(lines, labels, loc="upper left")
    ax_top.set_title(f"Locust {title_label}: Throughput, Active Users & Latency")

    ax_bot.plot(t, history["p50_ms"], color="#1a7f37", linewidth=2, label="p50 latency (ms)")
    ax_bot.plot(t, history["p95_ms"], color="#cf222e", linewidth=2, label="p95 latency (ms)")
    ax_bot.set_xlabel("Elapsed Time (seconds)")
    ax_bot.set_ylabel("Latency (ms)")
    ax_bot.legend(loc="upper left")
    ax_bot.grid(True, linestyle="--", alpha=0.5)

    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def run_profile(profile: str, base_url: str, out_dir: Path, locustfile: Path) -> dict:
    prefix = out_dir / f"locust_{profile}"
    env = os.environ.copy()
    env["LOCUST_PROFILE"] = profile

    cmd = [
        sys.executable,
        "-m",
        "locust",
        "-f",
        str(locustfile),
        "--headless",
        "--host",
        base_url,
        "--csv",
        str(prefix),
        "--csv-full-history",
        "--only-summary",
    ]
    print(f"\nRunning Locust profile '{profile}' against {base_url}...")
    proc = subprocess.run(cmd, env=env, check=False)

    stats_csv = Path(f"{prefix}_stats.csv")
    history_csv = Path(f"{prefix}_stats_history.csv")
    if not stats_csv.exists() or not history_csv.exists():
        raise RuntimeError(f"Locust profile '{profile}' exited ({proc.returncode}) without generating CSV reports")

    summary = load_stats_summary(stats_csv)
    history = load_history_series(history_csv)

    agg = summary["aggregated"] or {}
    total_reqs = agg.get("requests", 0)
    total_fails = agg.get("failures", 0)
    fail_rate = (total_fails / total_reqs * 100.0) if total_reqs > 0 else 0.0

    agg["failure_rate_pct"] = round(fail_rate, 3)
    agg["peak_users"] = max(history["users"]) if history["users"] else 0
    agg["peak_rps"] = round(max(history["rps"]) if history["rps"] else 0.0, 2)
    agg["duration_s"] = round(history["elapsed_s"][-1] if history["elapsed_s"] else 0.0, 1)
    agg["locust_exit_code"] = proc.returncode

    plot_name = "locust_load_test.png" if profile == "load" else "locust_spike_test.png"
    plot_profile(profile, history, out_dir / plot_name)

    print(
        f"Profile '{profile}' summary: requests={total_reqs}, failures={total_fails} ({fail_rate:.2f}%), "
        f"avg_rps={agg.get('rps', 0):.1f}, peak_rps={agg['peak_rps']:.1f}, "
        f"p50={agg.get('median_ms', 0):.1f} ms, p95={agg.get('p95_ms', 0):.1f} ms"
    )
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Locust sustained load and spike test suite")
    parser.add_argument("--host", default="http://localhost:8080", help="Server base URL")
    parser.add_argument("--output-dir", default="bench/results", help="Results directory")
    parser.add_argument(
        "--profiles",
        nargs="+",
        choices=["load", "spike"],
        default=["load", "spike"],
        help="Locust profiles to execute",
    )
    parser.add_argument("--index-threads", type=int, default=8, help="Threads for auto-indexing if index is empty")
    args = parser.parse_args()

    base_url = args.host.rstrip("/")
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    locustfile = Path(__file__).resolve().parent / "locustfile.py"

    try:
        ensure_index_ready(base_url, args.index_threads)
    except Exception as exc:
        print(f"Failed to verify or initialize index at {base_url}: {exc}", file=sys.stderr)
        return 1

    all_summaries = {}
    for profile in args.profiles:
        all_summaries[profile] = run_profile(profile, base_url, out_dir, locustfile)

    summary_path = out_dir / "locust_summary.json"
    summary_path.write_text(json.dumps(all_summaries, indent=2), encoding="utf-8")
    print(f"\nSaved Locust summary and plots to {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
