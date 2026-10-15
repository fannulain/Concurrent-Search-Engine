# Concurrent Search Engine

A containerized, multi-threaded search engine in C++20. Coursework for *Parallel Computing* (KPI, IASA, 2026–2027).

- A custom thread pool builds an inverted index in parallel.
- The inverted index is our own thread-safe data structure (striped locking).
- The server handles REST and WebSocket requests concurrently.
- A local LLM ([Ollama](https://ollama.com/)) expands queries.

The full specification is in [spec/srs.md](spec/srs.md). The UML diagrams are in [docs/diagrams](docs/diagrams).

> **Status:** First attestation. `docker compose up` starts the whole system, and `GET /api/health` is implemented. The other endpoints return `501 Not Implemented`.

## Architecture

| Service | Description | Host port |
|---|---|---|
| `server` | C++20 search server (Boost.Beast) | `8080` |
| `ollama` | Ollama LLM runtime | internal only |
| `ollama-init` | One-shot job that pulls the model (`qwen2.5:0.5b`) and exits | — |
| `web` | Nginx: Web UI + reverse proxy for `/api` and `/ws` | `3000` |

## Prerequisites

- Docker Desktop / Docker Engine with Docker Compose v2
- ~2 GB of free disk space (images + LLM model)

## Quick start

```bash
docker compose up --build
```

The first start takes a few minutes: the server image is compiled and the `ollama-init` service downloads the LLM model (~400 MB). Ollama reports `true` only after the model download has finished.

Check the server:

```bash
curl http://localhost:8080/api/health
# {"ollama":true,"status":"ok"}
```

Web UI: <http://localhost:3000>

Stop the system:

```bash
docker compose down        # keep the downloaded model
docker compose down -v     # also remove the model volume
```

## Configuration

These environment variables are set for the `server` service in [docker-compose.yml](docker-compose.yml):

| Variable | Default | Description |
|---|---|---|
| `PORT` | `8080` | HTTP port |
| `SERVER_THREADS` | number of CPU cores | Size of the network thread pool |
| `DATASET_PATH` | `/data` | Dataset directory (bind-mounted from `./data`, read-only) |
| `OLLAMA_URL` | `http://ollama:11434` | Ollama base URL |
| `OLLAMA_MODEL` | `qwen2.5:0.5b` | Model used for query expansion |

You can change the model with an env variable or a `.env` file, e.g. `OLLAMA_MODEL=llama3.2:1b docker compose up`.

## API (current state)

| Endpoint | Status |
|---|---|
| `GET /api/health` | ✅ `{"status": "ok", "ollama": true\|false}` |
| `POST /api/index` | ⏳ 501 |
| `POST /api/search` | ⏳ 501 |
| `GET /api/stats` | ⏳ 501 |
| `WS /ws/search` | ⏳ not available |

## Local build (without Docker)

You need a C++20 compiler, CMake ≥ 3.20, Boost ≥ 1.81 (headers) and nlohmann/json ≥ 3.11.

```bash
# Ubuntu 24.04
sudo apt install g++ cmake ninja-build libboost-dev nlohmann-json3-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
OLLAMA_URL=http://localhost:11434 ./build/search_server
```

To build with ThreadSanitizer, add `-DCSE_ENABLE_TSAN=ON`.

## Project structure

```
.
├── CMakeLists.txt          # C++ build
├── Dockerfile              # multi-stage server image
├── docker-compose.yml      # system topology
├── src/                    # C++ server sources
│   ├── main.cc             # entry point, network thread pool
│   ├── config.*            # env configuration
│   ├── thread_pool.*       # custom ThreadPool
│   ├── http_server.*       # Boost.Beast listener / session
│   ├── request_handler.*   # REST routing
│   └── ollama_client.*     # Ollama HTTP client
├── web/                    # Nginx config + Web UI
├── data/                   # dataset (not tracked in git)
├── docs/diagrams/          # UML diagrams (PlantUML + SVG)
└── spec/srs.md             # Software Requirements Specification
```

## Code style

- C++: [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html), enforced with `clang-format` (see [.clang-format](.clang-format)).
- Commits: [Conventional Commits](https://www.conventionalcommits.org/).