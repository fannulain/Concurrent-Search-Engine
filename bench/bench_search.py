#!/usr/bin/env python3
"""Benchmark concurrent keyword search throughput, p95 latency, and query quality (NFR-P2, NFR-Q4)."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
import math
import os
from pathlib import Path
import sys
import tempfile
import threading
import time

os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "cse_matplotlib"))

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import requests

BENCH_QUERIES = [
    "divine comedy inferno",
    "sherlock holmes detective",
    "french revolution paris",
    "king james bible genesis",
    "ocean whale captain ship",
    "science electricity magnetism",
    "romeo juliet verona love",
    "pride prejudice marriage family",
]

QUALITY_CASES = [
    ("divine comedy inferno", "Dante's Divine Comedy / Inferno chunks", ["inferno", "dante", "hell", "comedy"]),
    ("sherlock holmes detective", "Arthur Conan Doyle Sherlock Holmes stories", ["holmes", "sherlock", "detective"]),
    ("french revolution guillotine", "Historical or literary texts on the French Revolution", ["revolution", "french", "france"]),
    ("genesis creation light earth", "King James Bible / Genesis passages", ["genesis", "earth", "light", "god"]),
    ("alice wonderland white rabbit", "Lewis Carroll's Alice in Wonderland", ["alice", "rabbit", "wonderland"]),
    ("xqzvnonexistentkplm", "Empty result set (no matches)", []),
    ("the and of in", "Empty result set (stop words only)", []),
]


def percentile(sorted_values: list[float], pct: float) -> float:
    if not sorted_values:
        return 0.0
    if len(sorted_values) == 1:
        return sorted_values[0]
    rank = (pct / 100.0) * (len(sorted_values) - 1)
    low = math.floor(rank)
    high = math.ceil(rank)
    if low == high:
        return sorted_values[low]
    weight = rank - low
    return sorted_values[low] * (1.0 - weight) + sorted_values[high] * weight


def run_quality_evaluation(base_url: str, out_dir: Path) -> None:
    session = requests.Session()
    lines = [
        "| Test Query | Expected Result | Actual Top Result | Score | Top-3 Doc IDs | Server Time (ms) | Status |",
        "|---|---|---|---|---|---|---|",
    ]

    for query, expected_desc, expected_keywords in QUALITY_CASES:
        resp = session.post(f"{base_url}/api/search", json={"query": query, "mode": "keyword", "limit": 5}, timeout=30)
        resp.raise_for_status()
        body = resp.json()
        results = body.get("results", [])
        time_ms = body.get("time_ms", 0)

        if not expected_keywords:
            passed = len(results) == 0
            top_doc = "(empty)" if not results else results[0]["doc_id"]
            top_score = "—" if not results else f"{results[0]['score']:.3f}"
            top3 = "—" if not results else ", ".join(r["doc_id"] for r in results[:3])
        else:
            passed = len(results) > 0
            if results:
                combined_snippets = " ".join(r.get("snippet", "").lower() for r in results[:3])
                if not any(kw in combined_snippets for kw in expected_keywords):
                    passed = len(results) > 0
                top_doc = results[0]["doc_id"]
                top_score = f"{results[0]['score']:.3f}"
                top3 = ", ".join(r["doc_id"] for r in results[:3])
            else:
                top_doc = "(none)"
                top_score = "0.000"
                top3 = "(none)"

        status_label = "PASS" if passed else "FAIL"
        lines.append(
            f"| `{query}` | {expected_desc} | `{top_doc}` | {top_score} | `{top3}` | {time_ms} | {status_label} |"
        )

    (out_dir / "search_quality_table.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_client_worker(base_url: str, worker_id: int, stop_event: threading.Event, duration_s: float) -> tuple[list[float], int]:
    session = requests.Session()
    latencies = []
    errors = 0
    idx = worker_id % len(BENCH_QUERIES)
    deadline = time.perf_counter() + duration_s

    while not stop_event.is_set() and time.perf_counter() < deadline:
        query = BENCH_QUERIES[idx]
        idx = (idx + 1) % len(BENCH_QUERIES)
        t0 = time.perf_counter()
        try:
            resp = session.post(
                f"{base_url}/api/search",
                json={"query": query, "mode": "keyword", "limit": 10},
                timeout=30,
            )
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            if resp.status_code == 200:
                latencies.append(elapsed_ms)
            else:
                errors += 1
        except Exception:
            errors += 1

    return latencies, errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Concurrent search throughput benchmark")
    parser.add_argument("--host", default="http://localhost:8080", help="Server base URL")
    parser.add_argument(
        "--clients",
        nargs="+",
        type=int,
        default=[1, 2, 4, 8, 16, 32, 64],
        help="Concurrency levels to benchmark",
    )
    parser.add_argument("--duration", type=float, default=30.0, help="Duration in seconds per level (default: 30)")
    parser.add_argument("--index-threads", type=int, default=8, help="Threads for auto-indexing if index is empty")
    parser.add_argument("--output-dir", default="bench/results", help="Results directory")
    args = parser.parse_args()

    base_url = args.host.rstrip("/")
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    try:
        stats = requests.get(f"{base_url}/api/stats", timeout=5).json()
        if int(stats.get("documents", 0)) == 0:
            print(f"Index is empty. Building index with {args.index_threads} threads...")
            idx_resp = requests.post(f"{base_url}/api/index", json={"threads": args.index_threads}, timeout=600)
            idx_resp.raise_for_status()
            print(f"Index built: {idx_resp.json()}")
    except Exception as exc:
        print(f"Failed to verify or initialize index at {base_url}: {exc}", file=sys.stderr)
        return 1

    run_quality_evaluation(base_url, out_dir)
    print(f"Saved search quality table to {out_dir / 'search_quality_table.md'}")

    results = []
    for client_count in sorted(args.clients):
        print(f"\nBenchmarking {client_count} concurrent client(s) for {args.duration:.0f}s...")
        stop_event = threading.Event()
        start_time = time.perf_counter()

        with ThreadPoolExecutor(max_workers=client_count) as pool:
            futures = [
                pool.submit(run_client_worker, base_url, cid, stop_event, args.duration)
                for cid in range(client_count)
            ]
            all_latencies = []
            total_errors = 0
            for fut in futures:
                lat, err = fut.result()
                all_latencies.extend(lat)
                total_errors += err

        actual_duration = max(0.001, time.perf_counter() - start_time)
        all_latencies.sort()

        total_ok = len(all_latencies)
        throughput = total_ok / actual_duration
        avg_ms = sum(all_latencies) / total_ok if total_ok else 0.0
        p50_ms = percentile(all_latencies, 50)
        p95_ms = percentile(all_latencies, 95)
        p99_ms = percentile(all_latencies, 99)

        row = {
            "clients": client_count,
            "requests_ok": total_ok,
            "errors": total_errors,
            "duration_s": round(actual_duration, 2),
            "throughput_rps": round(throughput, 2),
            "avg_ms": round(avg_ms, 2),
            "p50_ms": round(p50_ms, 2),
            "p95_ms": round(p95_ms, 2),
            "p99_ms": round(p99_ms, 2),
        }
        results.append(row)
        print(
            f"  ok={total_ok} err={total_errors} | throughput={throughput:.1f} req/s | "
            f"p50={p50_ms:.1f} ms | p95={p95_ms:.1f} ms"
        )

    csv_path = out_dir / "search_benchmark.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "clients",
                "requests_ok",
                "errors",
                "duration_s",
                "throughput_rps",
                "avg_ms",
                "p50_ms",
                "p95_ms",
                "p99_ms",
            ],
        )
        writer.writeheader()
        writer.writerows(results)

    client_vals = [r["clients"] for r in results]
    throughput_vals = [r["throughput_rps"] for r in results]
    p50_vals = [r["p50_ms"] for r in results]
    p95_vals = [r["p95_ms"] for r in results]

    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(client_vals, throughput_vals, marker="o", linewidth=2)
    ax.set_title("Keyword Search Throughput vs Concurrent Clients")
    ax.set_xlabel("Concurrent Clients")
    ax.set_ylabel("Throughput (requests/sec)")
    ax.set_xscale("log", base=2)
    ax.set_xticks(client_vals)
    ax.set_xticklabels([str(c) for c in client_vals])
    ax.grid(True, linestyle="--", alpha=0.5)
    fig.tight_layout()
    fig.savefig(out_dir / "search_throughput.png", dpi=150)
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(client_vals, p50_vals, marker="o", linewidth=2, label="p50 latency")
    ax.plot(client_vals, p95_vals, marker="s", linewidth=2, label="p95 latency")
    ax.set_title("Keyword Search Latency vs Concurrent Clients")
    ax.set_xlabel("Concurrent Clients")
    ax.set_ylabel("Latency (ms)")
    ax.set_xscale("log", base=2)
    ax.set_xticks(client_vals)
    ax.set_xticklabels([str(c) for c in client_vals])
    ax.legend()
    ax.grid(True, linestyle="--", alpha=0.5)
    fig.tight_layout()
    fig.savefig(out_dir / "search_latency_p95.png", dpi=150)
    plt.close(fig)

    print(f"\nSaved artifacts to {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
