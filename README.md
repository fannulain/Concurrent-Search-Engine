# Concurrent Search Engine

A containerized, multi-threaded search engine in C++20 with parallel inverted index construction, striped locking, REST & WebSocket streaming APIs, and local LLM query expansion via Ollama. Coursework for *Parallel Computing* (KPI, IASA, 2026–2027).

- **Custom `ThreadPool` & `TaskQueue`:** Built from scratch on `std::thread`, `std::mutex`, and `std::condition_variable` with `std::future` support and graceful shutdown.
- **Custom `ConcurrentInvertedIndex`:** Striped locking across 64 cache-aligned `std::shared_mutex` buckets for parallel writes and lock-shared concurrent reads.
- **Atomic Snapshot Isolation:** Index rebuilds swap `std::atomic<std::shared_ptr<const IndexSnapshot>>` without blocking active searches.
- **Local LLM Query Expansion:** Sequential 1-thread Ollama worker (`/api/generate`) enriches queries with keywords and synonyms, streaming pipeline stages over `/ws/search`.
- **Dual Clients:** Interactive Web UI (Nginx + vanilla JS) and Python CLI client (`./search`).

The full specification is in [spec/srs.md](spec/srs.md). Architecture details are in [docs/architecture.md](docs/architecture.md), and UML diagrams are in [docs/diagrams](docs/diagrams).

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
- Optional (for CLI/benchmarks outside Docker): Python 3.10+ (`pip install -r requirements.txt`)

### Dataset Setup

If `./data/` is empty, download Project Gutenberg plain-text books with:

```bash
python3 scripts/download_dataset.py --target-gb 1.5 --output data
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

---

## Configuration

Environment variables configured for `server` in [docker-compose.yml](docker-compose.yml):

| Variable | Default | Description |
|---|---|---|
| `PORT` | `8080` | HTTP/WebSocket listen port |
| `SERVER_THREADS` | CPU core count | Network `ThreadPool` worker threads |
| `DATASET_PATH` | `/data` | Read-only dataset mount path |
| `OLLAMA_URL` | `http://ollama:11434` | Ollama base URL |
| `OLLAMA_MODEL` | `qwen2.5:0.5b` | Model used for query expansion |
| `OLLAMA_TIMEOUT_MS` | `30000` | HTTP timeout for `/api/generate` |
| `INDEX_BUCKETS` | `64` | Striped lock bucket count (power of two) |

---

## REST API & WebSocket Usage

### Health Check
```bash
curl http://localhost:8080/api/health
# {"ollama":true,"status":"ok"}
```

### Build Inverted Index
```bash
curl -X POST http://localhost:8080/api/index \
  -H "Content-Type: application/json" \
  -d '{"threads": 8}'
# {"documents_indexed":265412,"status":"ok","time_ms":4120}
```

### Get Index Statistics
```bash
curl http://localhost:8080/api/stats
# {"documents":265412,"index_build_time_ms":4120,"threads_used":8}
```

### Keyword Search (REST)
```bash
curl -X POST http://localhost:8080/api/search \
  -H "Content-Type: application/json" \
  -d '{"query": "divine comedy inferno", "mode": "keyword", "limit": 5}'
# {"results":[{"doc_id":"1001_0","score":1.0,"snippet":"..."}],"time_ms":14}
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
./search "space stars" --mode stream --limit 5 --host http://localhost:8080
```

---

## Building & Testing

### Via Docker (`test` target: clang-format + Release GoogleTest + ThreadSanitizer)
```bash
docker build --target test --progress=plain .
```

### Local Build (Ubuntu 24.04)
```bash
sudo apt install g++ cmake ninja-build libboost-dev nlohmann-json3-dev libgtest-dev clang-format

# Release build + unit tests
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCSE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# ThreadSanitizer (TSan) build + concurrency tests
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCSE_BUILD_TESTS=ON -DCSE_ENABLE_TSAN=ON
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
```

---

## Scalability Benchmarks

Install Python dependencies and run the benchmark suite against a running server:

```bash
pip install -r requirements.txt
python3 bench/bench_index.py --host http://localhost:8080
python3 bench/bench_search.py --host http://localhost:8080
```

CSV metrics, PNG plots (`index_build_time.png`, `index_speedup_efficiency.png`, `search_throughput.png`, `search_latency_p95.png`), and `search_quality_table.md` are generated in `bench/results/`.

---

## Project Structure

```text
.
├── .github/workflows/ci.yml      # GitHub Actions (format, Release, TSan)
├── CMakeLists.txt                # C++20 build & GoogleTest configuration
├── Dockerfile                    # Multi-stage build, test (TSan), runtime
├── docker-compose.yml            # Full stack topology
├── search                        # Python CLI search client
├── bench/                        # Scalability & quality benchmark scripts
├── scripts/                      # Project Gutenberg dataset downloader
├── src/
│   ├── Source.cpp                # Application entry point
│   ├── Config/                   # Environment configuration
│   ├── ThreadPool/               # Custom TaskQueue & ThreadPool
│   ├── Index/                    # Tokenizer, DocumentStore, ConcurrentInvertedIndex, IndexerService
│   ├── Search/                   # TF-IDF SearchEngine & snippet extractor
│   ├── Ollama/                   # Ollama HTTP client & single-thread expansion queue
│   └── Server/                   # SearchServer, RequestHandler, ClientSession, WebSocketSession
├── tests/                        # GoogleTest unit & concurrency tests
├── web/                          # Nginx reverse proxy & Web UI static files
├── docs/                         # Architecture docs, benchmarks, UML diagrams
└── spec/srs.md                   # Software Requirements Specification
```

---

## Code Style & Commits

- **C++:** Unified Microsoft/Allman style (`PascalCase` files/classes, `camelCase` methods/variables, 4-space indentation, Allman braces, 120-column limit) verified by [`.clang-format`](.clang-format).
- **Commits:** Atomic [Conventional Commits](https://www.conventionalcommits.org/).
