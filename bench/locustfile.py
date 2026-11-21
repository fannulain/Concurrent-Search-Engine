"""Locust load and spike test definitions for Concurrent Search Engine."""

import os
import random
from locust import HttpUser, LoadTestShape, between, task

BENCH_QUERIES = [
    "divine comedy inferno",
    "sherlock holmes detective",
    "french revolution guillotine",
    "moses pharaoh egypt exodus",
    "ocean whale captain ship",
    "science electricity magnetism",
    "romeo juliet verona love",
    "pride prejudice marriage family",
]


class SearchEngineUser(HttpUser):
    wait_time = between(0.05, 0.2)

    def on_start(self) -> None:
        self.query_idx = random.randrange(len(BENCH_QUERIES))

    @task(10)
    def keyword_search(self) -> None:
        query = BENCH_QUERIES[self.query_idx]
        self.query_idx = (self.query_idx + 1) % len(BENCH_QUERIES)
        with self.client.post(
            "/api/search",
            json={"query": query, "mode": "keyword", "limit": 10},
            name="POST /api/search [keyword]",
            catch_response=True,
            timeout=30,
        ) as resp:
            if resp.status_code != 200:
                resp.failure(f"HTTP {resp.status_code}")
                return
            try:
                body = resp.json()
            except ValueError as exc:
                resp.failure(f"Invalid JSON: {exc}")
                return
            if "results" not in body or "time_ms" not in body:
                resp.failure("Missing results/time_ms fields")

    @task(1)
    def get_stats(self) -> None:
        with self.client.get("/api/stats", name="GET /api/stats", catch_response=True, timeout=10) as resp:
            if resp.status_code != 200:
                resp.failure(f"HTTP {resp.status_code}")
                return
            try:
                body = resp.json()
            except ValueError as exc:
                resp.failure(f"Invalid JSON: {exc}")
                return
            if int(body.get("documents", 0)) <= 0:
                resp.failure("Index is empty")

    @task(1)
    def get_health(self) -> None:
        with self.client.get("/api/health", name="GET /api/health", catch_response=True, timeout=10) as resp:
            if resp.status_code != 200:
                resp.failure(f"HTTP {resp.status_code}")
                return
            try:
                body = resp.json()
            except ValueError as exc:
                resp.failure(f"Invalid JSON: {exc}")
                return
            if body.get("status") != "ok":
                resp.failure(f"Unexpected status: {body.get('status')}")


class CustomLoadShape(LoadTestShape):
    LOAD_STAGES = [
        (20, 25, 5),
        (50, 50, 5),
        (90, 100, 10),
        (110, 150, 15),
        (120, 20, 20),
    ]

    SPIKE_STAGES = [
        (20, 10, 5),
        (45, 200, 100),
        (65, 10, 50),
        (90, 250, 125),
        (110, 10, 50),
    ]

    def __init__(self) -> None:
        super().__init__()
        self.profile = os.environ.get("LOCUST_PROFILE", "load").strip().lower()

    def tick(self) -> tuple[int, float] | None:
        run_time = self.get_run_time()
        stages = self.SPIKE_STAGES if self.profile == "spike" else self.LOAD_STAGES
        for time_limit, users, spawn_rate in stages:
            if run_time < time_limit:
                return (users, float(spawn_rate))
        return None
