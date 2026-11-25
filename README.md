# Concurrent Search Engine

A containerized, multi-threaded search engine in C++20 with parallel inverted index construction, striped locking, Okapi BM25 ranking, REST & WebSocket streaming APIs, and local LLM query expansion via Ollama. Coursework for *Parallel Computing* (KPI, IASA, 2026) completed by Dolzhenkov Yehor KN-32.

- **Custom `ThreadPool` & `TaskQueue`:** Built from scratch on `std::thread`, `std::mutex`, and `std::condition_variable` with `std::future` support, serialized `stop()` synchronization, and single-source-of-truth queue state.
- **Custom `ConcurrentInvertedIndex` & Sealed `DocumentStore`:** Striped locking across 64 cache-aligned `std::shared_mutex` buckets, LPT file scheduling, 16-chunk batched ingestion (`addBatch` / `addPostingBatch`), and lock-free reads once `DocumentStore::seal()` completes.
- **Atomic Snapshot Isolation & Okapi BM25 Search:** Index rebuilds swap `std::atomic<std::shared_ptr<const IndexSnapshot>>` without blocking active readers; searches use zero-copy `visitPostings()`, a `thread_local` sparse score accumulator, and Okapi BM25 ($k_1 = 1.2, b = 0.75$) normalized to `[0, 1]`.
- **Local LLM Query Expansion:** Sequential 1-thread Ollama worker (`/api/generate`) with clean in-flight cancellation on shutdown enriches queries with keywords and synonyms, streaming pipeline stages over `/ws/search`.
- **Dual Clients:** Interactive Web UI (Nginx + vanilla JS) and Python CLI client (`./search`).

- **User Guide:** [docs/user_guide.md](docs/user_guide.md)
- **Specification (SRS):** [spec/srs.md](spec/srs.md)
- **Architecture Guide:** [docs/architecture.md](docs/architecture.md)
- **Benchmark & Load Report:** [docs/benchmarks.md](docs/benchmarks.md)
- **UML Diagrams:** [docs/diagrams](docs/diagrams)

---

## Architecture

| Service | Description | Host Port |
|---|---|---|
| `server` | C++20 search server (Boost.Beast HTTP + WebSocket) | `8080` |
| `ollama` | Ollama LLM runtime (`qwen2.5:0.5b`) | internal only |
| `ollama-init` | One-shot container pulling `OLLAMA_MODEL` | — |
| `web` | Nginx serving Web UI + proxying `/api` and `/ws` | `3000` |

---

## Prerequisites

- Docker Desktop / Docker Engine with Docker Compose v2
- ~2 GB free disk space (container images + `qwen2.5:0.5b` weights)
- English `.txt` dataset placed under `./data/` (minimum 1 GB, >= 10,000 chunks; see below)
- Optional (for CLI/tests/benchmarks outside Docker): Python 3.10+ (`pip install -r requirements.txt`)

### Dataset Setup

If `./data/` is empty, download Project Gutenberg plain-text books with:

```bash
python scripts/download_dataset.py --target-gb 1.5 --output data
```

Any `.txt` files placed under `./data/` (including subdirectories such as `data/D1.7GB/`) are scanned recursively, stripped of Project Gutenberg license headers/footers on the fly, and split into ~1,000-word searchable document chunks.

---

## Quick Start (Docker Compose)

```bash
docker compose up --build
```

The first startup compiles the C++ server and pulls the `qwen2.5:0.5b` model (~400 MB) in `ollama-init`.

- **Web UI:** <http://localhost:3000>
- **Server API:** <http://localhost:8080>

Stop the stack:

```bash
docker compose down        # preserve downloaded Ollama models
docker compose down -v     # also remove model volume
```

For full walkthroughs and troubleshooting, see [docs/user_guide.md](docs/user_guide.md).

---

## Configuration

Environment variables configured for `server` in [docker-compose.yml](docker-compose.yml):

| Variable | Default | Description |
|---|---|---|
| `PORT` | `8080` | HTTP/WebSocket listen port |
| `SERVER_THREADS` | CPU core count (min `4`) | Network `ThreadPool` worker threads |
| `DATASET_PATH` | `/data` | Read-only dataset mount path |
| `OLLAMA_URL` | `http://ollama:11434` | Ollama base URL |
| `OLLAMA_MODEL` | `qwen2.5:0.5b` | Model used for query expansion |
| `OLLAMA_TIMEOUT_MS` | `30000` | HTTP timeout for `/api/generate` |
| `INDEX_BUCKETS` | `64` | Striped lock bucket count (power of two) |
| `MALLOC_ARENA_MAX` | `16` | Bounds glibc malloc arenas during 1–128 thread indexing |

---

## REST API & WebSocket Usage

### Health Check
```bash
curl http://localhost:8080/api/health
# {"ollama":true,"status":"ok"}
```

### Build Inverted Index (`threads`: `1–128`)
```bash
curl -X POST http://localhost:8080/api/index \
  -H "Content-Type: application/json" \
  -d '{"threads": 8}'
# {"documents_indexed":200001,"status":"ok","time_ms":37038}
```

### Get Index Statistics
```bash
curl http://localhost:8080/api/stats
# {"documents":200001,"index_build_time_ms":37038,"threads_used":8}
```

### Keyword Search (REST)
```bash
curl -X POST http://localhost:8080/api/search \
  -H "Content-Type: application/json" \
  -d '{"query": "divine comedy inferno", "mode": "keyword", "limit": 5}'
# {"results":[{"doc_id":"1003_40","score":1.0,"snippet":"..."}],"time_ms":50}
```

### Streaming Search (WebSocket `/ws/search`)
Send `{"query": "divine comedy inferno"}` to `ws://localhost:8080/ws/search` (or `ws://localhost:3000/ws/search`). The server streams three sequential JSON messages:
1. `{"stage": "query_expansion", "data": {"original": "...", "expanded_terms": [...]}}`
2. `{"stage": "keyword_search", "data": {"results": [...], "time_ms": ...}}`
3. `{"stage": "done"}`

---

## CLI Client

```bash
./search "divine comedy inferno"
./search "divine comedy inferno" --mode stream --limit 5 --host http://localhost:8080
```

On Windows, run with `python search "divine comedy inferno"`.

---

## Building & Testing

### Via Docker (`test` target: `clang-format` + 30 Release GoogleTests + 30 ThreadSanitizer Tests)
```bash
docker build --target test -t cse-test --progress=plain .
docker run --rm --security-opt seccomp=unconfined cse-test
```

### Python CLI Unit Tests
```bash
python -m unittest discover -s tests -p "test_*.py" -v
```

### Local Build (Ubuntu 24.04)
```bash
sudo apt install g++ clang libclang-rt-18-dev cmake ninja-build libboost-dev nlohmann-json3-dev libgtest-dev clang-format

# Release build + unit tests
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCSE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# ThreadSanitizer (TSan) build + concurrency tests
cmake -S . -B build-tsan -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCSE_BUILD_TESTS=ON -DCSE_ENABLE_TSAN=ON
cmake --build build-tsan --parallel
setarch x86_64 -R ctest --test-dir build-tsan --output-on-failure
```

---

## Scalability, Load & Spike Benchmarks

Install Python dependencies and run the full benchmark suite against a running server (`docker compose up -d --build`):

```bash
pip install -r requirements.txt
python bench/bench_index.py --host http://localhost:8080 --threads 1 2 4 8 16 32 64 128 --runs 5
python bench/bench_search.py --host http://localhost:8080 --clients 1 2 4 8 16 32 64 --duration 30 --data-dir data
python bench/run_locust_suite.py --host http://localhost:8080
```

CSV metrics, PNG plots (`index_build_time.png`, `index_speedup_efficiency.png`, `search_throughput.png`, `search_latency_p95.png`, `locust_load_test.png`, `locust_spike_test.png`), `locust_summary.json`, and `search_quality_table.md` are generated in `bench/results/`.

Measured scalability results (`1–128` threads), Locust load/spike performance, search quality evaluation, and bottleneck analysis are documented in [docs/benchmarks.md](docs/benchmarks.md).

---

## Project Structure

```text
.
├── .github/workflows/ci.yml      # GitHub Actions (format, Release, TSan, Python CLI tests)
├── CMakeLists.txt                # C++20 build & GoogleTest configuration
├── Dockerfile                    # Multi-stage build, test (TSan), runtime
├── docker-compose.yml            # Full stack topology
├── search                        # Python CLI search client
├── bench/                        # Index (1-128), search, and Locust load/spike suites + results
├── scripts/                      # Project Gutenberg dataset downloader
├── src/
│   ├── Source.cpp                # Application entry point
│   ├── Config/                   # Environment configuration
│   ├── ThreadPool/               # Custom TaskQueue & ThreadPool
│   ├── Index/                    # Tokenizer, DocumentStore, ConcurrentInvertedIndex, IndexerService
│   ├── Search/                   # Okapi BM25 SearchEngine & snippet extractor
│   ├── Ollama/                   # Ollama HTTP client & single-thread expansion queue
│   └── Server/                   # SearchServer, RequestHandler, ClientSession, WebSocketSession
├── tests/                        # GoogleTest (C++/TSan) & Python CLI unit tests
├── web/                          # Nginx reverse proxy & Web UI static files
├── docs/                         # User guide, architecture docs, benchmarks, UML diagrams
└── spec/srs.md                   # Software Requirements Specification
```

---

## Code Style & Commits

- **C++:** Unified Microsoft/Allman style (`PascalCase` files/classes, `camelCase` methods/variables, 4-space indentation, Allman braces, 120-column limit) verified by [`.clang-format`](.clang-format).
- **Commits:** Atomic [Conventional Commits](https://www.conventionalcommits.org/).
