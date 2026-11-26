# User Guide — Concurrent Search Engine

This guide walks through setting up, running, querying, testing, and benchmarking the **Concurrent Search Engine**.

---

## 1. Prerequisites

- **Docker Engine / Docker Desktop** with **Docker Compose v2**
- **Disk space:** ~2 GB for container images and `qwen2.5:0.5b` model weights, plus space for your English `.txt` corpus (minimum 1 GB recommended)
- **Python 3.10+** (only needed for dataset downloading, the CLI client, and benchmark/load-testing scripts outside Docker)

Install Python dependencies:

```bash
pip install -r requirements.txt
```

> **Windows note:** Use `python` (not the Microsoft Store `python3` alias) when running scripts on Windows.

---

## 2. Preparing the Dataset

Place English plain-text `.txt` files anywhere inside `./data/` (subdirectories such as `data/D1.7GB/` are scanned recursively).

If `./data/` is empty, download Project Gutenberg books automatically:

```bash
python scripts/download_dataset.py --target-gb 1.5 --output data
```

During indexing, `IndexerService` automatically detects and strips Project Gutenberg license headers (`*** START OF ... ***`) and footers (`*** END OF ... ***`), and splits each book into ~1,000-word searchable document chunks (`<book_id>_<chunk_no>`).

---

## 3. Starting and Stopping the System

### Start All Services

```bash
docker compose up --build
```

Or run in the background:

```bash
docker compose up -d --build
```

Docker Compose launches four containers:

| Service | Purpose | Host Port |
|---|---|---|
| `server` | C++20 search server (REST + WebSocket) | `8080` |
| `web` | Nginx Web UI + reverse proxy (`/api/`, `/ws/`) | `3000` |
| `ollama` | Local Ollama LLM runtime | internal (`11434`) |
| `ollama-init` | One-shot container pulling `OLLAMA_MODEL` (`qwen2.5:0.5b`) | — |

### Verify Readiness

Check system status until `ollama` becomes `true` (on first launch, `ollama-init` pulls ~400 MB):

```bash
curl http://localhost:8080/api/health
# {"ollama":true,"status":"ok"}
```

Inspect container logs or status at any time:

```bash
docker compose ps
docker compose logs -f server ollama-init
```

### Stop Services

```bash
docker compose down        # keep downloaded Ollama models in named volume
docker compose down -v     # remove Ollama model volume as well
```

---

## 4. Using the Web UI (`http://localhost:3000`)

1. Open **<http://localhost:3000>** in your browser.
2. **System Status Cards:** Verify `Search Server: OK`, `Ollama LLM: available`, and the top-right badge shows `WS: connected`.
3. **Build the Index:**
   - In the **Index Builder** panel, enter the desired worker thread count (`1–128`, e.g., `8`).
   - Click **Build Index**. The button disables while `POST /api/index` runs and displays the indexed chunk count and build time once complete.
4. **Search:**
   - Enter a query (e.g., `divine comedy inferno`).
   - Choose result limit (`Top 5`, `10`, `20`, or `50`).
   - Select mode:
     - **WebSocket Streaming (LLM):** Streams stages sequentially across `/ws/search` (`1. query_expansion` showing LLM-added terms → `2. keyword_search` showing BM25 execution time → `3. done`). If Ollama is still pulling or unavailable, a fallback banner appears and search proceeds with original query terms.
     - **REST Keyword Only:** Calls `POST /api/search` directly without invoking the LLM.
   - Inspect ranked results with document IDs, normalized `[0, 1]` BM25 scores, and highlighted ~200-character snippets.

---

## 5. Using the CLI Client (`search`)

On Linux/macOS:

```bash
./search "divine comedy inferno"
./search "divine comedy inferno" --mode stream --limit 5
```

On Windows (or via explicit interpreter):

```bash
python search "divine comedy inferno"
python search "divine comedy inferno" --mode stream --limit 5 --host http://localhost:8080
```

### CLI Options

| Flag | Default | Description |
|---|---|---|
| `query` | *(required)* | Search query string |
| `--mode` | `keyword` | `keyword` (REST `POST /api/search`) or `stream` (WebSocket `/ws/search` with LLM expansion) |
| `--limit` | `10` | Number of Top-K results (`1–100`) |
| `--host` | `http://localhost:8080` | Base URL of the search server (or `http://localhost:3000` via Nginx) |

Exit status is `0` on success and non-zero (`1`) on validation, HTTP, or connection errors.

---

## 6. REST & WebSocket API Reference

| Method & Path | Request Body | Example Response | Status Codes |
|---|---|---|---|
| `GET /api/health` | — | `{"ollama":true,"status":"ok"}` | `200` |
| `POST /api/index` | `{"threads": 8}` (`1..128`) | `{"documents_indexed":200001,"status":"ok","time_ms":21930}` | `200`, `400`, `409`, `500` |
| `GET /api/stats` | — | `{"documents":200001,"index_build_time_ms":21930,"threads_used":8}` | `200` |
| `POST /api/search` | `{"query":"inferno","mode":"keyword","limit":10}` | `{"results":[{"doc_id":"1003_40","score":1.0,"snippet":"..."}],"time_ms":18}` | `200`, `400` |
| `WS /ws/search` | `{"query":"inferno","limit":10}` | Streams `query_expansion`, `keyword_search`, `done` (or `error`) frames | `101`, `404` |

```bash
curl -X POST http://localhost:8080/api/index \
  -H "Content-Type: application/json" \
  -d '{"threads": 8}'

curl -X POST http://localhost:8080/api/search \
  -H "Content-Type: application/json" \
  -d '{"query": "divine comedy inferno", "mode": "keyword", "limit": 5}'
```

---

## 7. Running Automated Tests

### C++ Formatting, Release Tests, and ThreadSanitizer (Docker)

```bash
docker build --target test -t cse-test --progress=plain .
docker run --rm --security-opt seccomp=unconfined cse-test
```

This verifies `clang-format --dry-run --Werror`, builds and runs all 38 Release GoogleTest cases (including `ContainersTest`), and runs all 38 tests under Clang ThreadSanitizer (`-fsanitize=thread`).

### Local Linux Build & Test (Ubuntu 24.04)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCSE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

cmake -S . -B build-tsan -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCSE_BUILD_TESTS=ON -DCSE_ENABLE_TSAN=ON
cmake --build build-tsan --parallel
setarch x86_64 -R ctest --test-dir build-tsan --output-on-failure
```

### Python CLI Tests

```bash
python -m unittest discover -s tests -p "test_*.py" -v
```

---

## 8. Running Scalability, Load & Spike Benchmarks

Ensure the stack is running (`docker compose up -d --build`), then run:

### Index Build Scalability (1–128 Threads)

```bash
python bench/bench_index.py --host http://localhost:8080 --threads 1 2 4 8 16 32 64 128 --runs 5
```

Outputs `index_benchmark.csv`, `index_summary.csv`, `hardware_info.json`, `index_build_time.png`, and `index_speedup_efficiency.png` in `bench/results/`.

### Concurrent Search & Quality Evaluation (1–64 Clients)

```bash
python bench/bench_search.py --host http://localhost:8080 --clients 1 2 4 8 16 32 64 --duration 30 --data-dir data
```

Outputs `search_benchmark.csv`, `search_quality_table.md`, `search_throughput.png`, and `search_latency_p95.png` in `bench/results/`.

### Locust Sustained Load & Traffic Spike Suite

```bash
python bench/run_locust_suite.py --host http://localhost:8080
```

Runs both `load` (ramping to 150 users over 120 s) and `spike` (bursts to 200 and 250 users with recovery over 110 s) profiles and generates `locust_summary.json`, `locust_load_test.png`, and `locust_spike_test.png` in `bench/results/`.

To launch Locust's interactive web dashboard (`http://localhost:8089`) manually:

```bash
python -m locust -f bench/locustfile.py --host http://localhost:8080
```

---

## 9. Configuration Reference (`docker-compose.yml`)

| Variable | Default | Description |
|---|---|---|
| `PORT` | `8080` | HTTP/WebSocket listen port |
| `SERVER_THREADS` | CPU core count (min `4`) | Network `ThreadPool` worker threads |
| `DATASET_PATH` | `/data` | Read-only dataset directory inside container |
| `OLLAMA_URL` | `http://ollama:11434` | Ollama API base URL |
| `OLLAMA_MODEL` | `qwen2.5:0.5b` | Model used for `/api/generate` query expansion |
| `OLLAMA_TIMEOUT_MS` | `30000` | Timeout in ms for Ollama `/api/generate` |
| `INDEX_BUCKETS` | `64` | Striped lock bucket count (must be a power of two) |
| `MALLOC_ARENA_MAX` | `16` | Bounds glibc malloc arenas during high-thread indexing |

---

## 10. Troubleshooting

- **`"ollama": false` on `/api/health`:** Wait 1–2 minutes on first startup while `ollama-init` finishes pulling `qwen2.5:0.5b` (`docker compose logs -f ollama-init`). WebSocket search still works via automatic fallback to original query terms.
- **`409 Conflict` from `POST /api/index`:** Another index build is already running; wait for it to finish.
- **Empty search results right after startup:** The index starts empty in RAM and must be built once via the Web UI (**Build Index**) or `POST /api/index`.
- **High memory usage when indexing:** Ensure `MALLOC_ARENA_MAX=16` is set in `docker-compose.yml` and Docker Desktop has at least 6–8 GB of RAM allocated.
