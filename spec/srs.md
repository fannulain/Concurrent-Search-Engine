# Software Requirements Specification (SRS)
## Concurrent Search Engine with LLM Integration


## 1. Introduction

### 1.1 Purpose
This document defines the software requirements and architectural specification for a high-performance, containerized search engine. The system demonstrates parallel computing techniques at every stage: parallel construction of an inverted index via a custom thread pool, a custom thread-safe inverted index data structure, and concurrent handling of client requests. A locally deployed Large Language Model (Ollama) is used for query expansion.

### 1.2 Scope
The system is a multi-threaded C++ backend server that:
1. Indexes an English text dataset (volume ≥ 1 GB, ≥ 10,000 documents) in parallel using a configurable number of threads $N$.
2. Serves concurrent search requests over a fixed REST API contract and a WebSocket streaming endpoint.
3. Expands search queries with keywords and synonyms through Ollama `/api/generate`.
4. Is accessed via two different clients: a Web UI and a Python CLI.
5. Is started with a single command: `docker compose up`.

### 1.3 Definitions and References
See Section 11 (Glossary). Primary references:
- C. Manning, *Introduction to Information Retrieval*, Ch. 4 "Index construction", Ch. 6 "Scoring, term weighting".
- A. Williams, *C++ Concurrency in Action*.
- Ollama API documentation: https://github.com/ollama/ollama/blob/main/docs/api.md
- Conventional Commits: https://www.conventionalcommits.org/
- Microsoft C++ / Allman formatting specification (enforced via `.clang-format`).

---

## 2. Users and Actors

1. **Client (Searcher):** An end user who submits search queries via the Web UI or the Python CLI and receives ranked documents.
2. **Administrator (Researcher):** A power user who triggers dataset indexing (specifying the number of threads $N$), checks system health, gathers statistics and runs scalability benchmarks.
3. **Ollama LLM (External System):** A locally deployed containerized LLM service used exclusively for query expansion and checked during health checks.

---

## 3. Technology Stack and Constraints

### 3.1 Technology Stack
| Layer | Technology |
|---|---|
| Backend server | C++20, CMake, Boost.Beast / Boost.Asio (HTTP + WebSocket), nlohmann/json |
| Web UI | HTML / CSS / vanilla JavaScript, served by Nginx |
| CLI client | Python 3 (`./search "query"`) |
| LLM | Ollama, model `qwen2.5:0.5b` (configurable via env `OLLAMA_MODEL`) |
| Containerization | Docker, Docker Compose |
| Testing | GoogleTest, ThreadSanitizer (TSan), Python `unittest` + Locust load/spike suite, benchmark scripts |
| Source control | Git / GitHub, Conventional Commits |

### 3.2 Design Constraints
- **CON-1:** The backend MUST be written in a compiled language (C++).
- **CON-2:** The thread pool (`ThreadPool`) MUST be implemented from scratch on top of `std::thread`, `std::mutex`, `std::condition_variable`. Using `std::async`, OpenMP, TBB, Boost thread pools or similar ready-made executors for indexing is prohibited.
- **CON-3:** The inverted index data structure with concurrent access MUST be implemented from scratch. Ready-made concurrent containers (TBB, folly, libcds, etc.) are prohibited. Standard containers (`std::unordered_map`, `std::vector`) and synchronization primitives (`std::shared_mutex`, `std::atomic`) are allowed as building blocks.
- **CON-4:** Third-party libraries are allowed only for networking (Boost.Beast/Asio), JSON (nlohmann/json) and testing (GoogleTest).
- **CON-5:** The REST API and WebSocket message formats are a **fixed contract** (Section 6) and MUST NOT be changed. Optional extra fields are allowed only where explicitly stated.
- **CON-6:** The two clients MUST use two different approaches: a web application and a console application.

---

## 4. System Architecture Overview

### 4.1 Components
| Component | Responsibility |
|---|---|
| `SearchServer` | Boost.Asio acceptor & signal handling, manages the network `ThreadPool` |
| `ClientSession` / `WebSocketSession` | Boost.Beast HTTP and WebSocket session handlers |
| `RequestHandler` | REST routing, request validation, JSON serialization, owns the active index snapshot |
| `ThreadPool` / `TaskQueue` | Custom fixed-size pool of worker threads with a thread-safe `TaskQueue`, serialized `stop()` synchronization, and `std::future` support |
| `IndexerService` | Recursively scans the dataset, sorts files LPT-descending by size, strips Gutenberg markers, batches up to 16 chunks, distributes tasks across an Indexer `ThreadPool` with $N$ threads, and seals the snapshot |
| `Tokenizer` | Text normalization: lowercase, strip punctuation/digits, stop-word filtering |
| `ConcurrentInvertedIndex` | Custom thread-safe inverted index with striped locking, `addPostingBatch`, `visitPostings`, and `shrinkToFit` (Section 7.1) |
| `DocumentStore` | Thread-safe registry of document metadata (`docId`, `filePath`, `byteOffset`, `byteLength`, `tokenCount`) supporting `addBatch` and lock-free reads once `seal()` is called |
| `SearchEngine` | Executes keyword search over an index snapshot using zero-copy `visitPostings`, thread-local sparse score buffers, Okapi BM25 scoring, and Top-K snippets |
| `OllamaService` | HTTP client for Ollama; query expansion via `/api/generate`; availability check; 1-thread worker queue with immediate in-flight cancellation via `stop()` |
| `Config` | Parses environment variables into server, dataset, index, and Ollama settings |
| Web UI (Nginx) | Search form, results list, visualization of pipeline stages; reverse-proxies `/api` and `/ws` to the server |
| CLI client (Python) | Command-line search (`./search`) via REST (`keyword`) and WebSocket (`stream`) |

### 4.2 UML Diagrams
All diagrams are stored in `docs/diagrams/`: PlantUML sources in `md/`, rendered images in `svg/`:

| Diagram | File |
|---|---|
| Use case | [`uml_use_case_diagram`](../docs/diagrams/md/uml_use_case_diagram.md) |
| Class | [`uml_class_diagram`](../docs/diagrams/md/uml_class_diagram.md) |
| Sequence — UC-1 Health Check | [`sequence_diagram_health`](../docs/diagrams/md/sequence_diagram_health.md) |
| Sequence — UC-2 Build Index | [`sequence_diagram_inverted_index`](../docs/diagrams/md/sequence_diagram_inverted_index.md) |
| Sequence — UC-3 Get Statistics | [`sequence_diagram_stats`](../docs/diagrams/md/sequence_diagram_stats.md) |
| Sequence — UC-4 REST Keyword Search | [`sequence_diagram_keyword_search`](../docs/diagrams/md/sequence_diagram_keyword_search.md) |
| Sequence — UC-5 WebSocket Streaming Search | [`sequence_diagram_websocket_pipeline`](../docs/diagrams/md/sequence_diagram_websocket_pipeline.md) |
| Deployment (Docker Compose topology) | [`uml_deployment_diagram`](../docs/diagrams/md/uml_deployment_diagram.md) |

---

## 5. Functional Requirements

### 5.1 Use Cases

#### UC-1: System Health Check (`GET /api/health`)
- **Actor:** Administrator
- **Description:** Verifies that the backend server is running and checks the availability of Ollama.
- **Main flow:** The server sends `GET http://ollama:11434/api/tags` with a 2 s timeout and responds with `200 OK`.
- **Requirements:**
  - **FR-1.1:** The endpoint MUST always return HTTP 200 while the server process is alive, regardless of Ollama availability.
  - **FR-1.2:** The field `ollama` MUST be `true` only if Ollama responded successfully and the configured model is present in the model list.

#### UC-2: Build Inverted Index (`POST /api/index`)
- **Actor:** Administrator
- **Description:** Triggers parallel indexing of the dataset with $N$ threads.
- **Main flow:**
  1. The server validates `threads` ($1 \le N \le 128$).
  2. `IndexerService` scans the dataset directory and sorts files descending by size (LPT scheduling), then creates an Indexer `ThreadPool` with exactly $N$ threads and a new `IndexSnapshot`.
  3. Each book file becomes one task (see Section 8 for splitting into documents).
  4. Workers tokenize chunks (`tokenizeToTermFreq`), batch up to 16 chunks into `DocumentStore::addBatch` and `ConcurrentInvertedIndex::addPostingBatch`, inserting postings into the **new** `ConcurrentInvertedIndex`.
  5. After all futures complete, the pool is joined and destroyed, `index.shrinkToFit()` compacts posting vectors, and `docs.seal()` enables lock-free search reads.
  6. The new index is published by an atomic snapshot swap (Section 7.3) followed by `malloc_trim(0)`. Statistics are updated.
  7. The server returns the number of indexed documents and the build time.
- **Alternative flows:**
  - **A1:** Invalid body or `threads` out of range → `400 Bad Request`.
  - **A2:** Another build is already in progress → `409 Conflict`.
  - **A3:** Dataset directory missing or empty → `500 Internal Server Error` with an `error` message.
- **Requirements:**
  - **FR-2.1:** `time_ms` MUST measure wall-clock time from the start of the dataset scan to the completion of the swap (using `std::chrono::steady_clock`).
  - **FR-2.2:** Build time MUST decrease as `threads` grows (up to the number of physical cores), which is verified by the scalability research (Section 10).
  - **FR-2.3:** Searches executed during a build MUST continue to use the previous index snapshot without blocking.
  - **FR-2.4:** The resulting index MUST be identical (same terms, postings and tf values) for any value of $N$.

#### UC-3: Get Statistics (`GET /api/stats`)
- **Actor:** Administrator
- **Description:** Returns metrics of the latest successful index build.
- **Requirements:**
  - **FR-3.1:** Before the first build the endpoint returns `{"documents": 0, "index_build_time_ms": 0, "threads_used": 0}`.

#### UC-4: REST Keyword Search (`POST /api/search`)
- **Actor:** Client
- **Description:** Synchronous keyword search over the inverted index. **Does not** invoke the LLM.
- **Main flow:** Validate request → tokenize query with the same `Tokenizer` as indexing → inspect postings in-place via `visitPostings` under shared locks → compute Okapi BM25 scores using lock-free sealed `DocumentStore` metadata → take Top-K → build snippets → respond.
- **Alternative flows:**
  - **A1:** Empty `query`, `mode` other than `"keyword"`, or `limit` outside $[1, 100]$ → `400 Bad Request`.
  - **A2:** Index not built yet → `200 OK` with empty `results`.
  - **A3:** Query contains only stop words → `200 OK` with empty `results`.
- **Requirements:**
  - **FR-4.1:** Results MUST be sorted by `score` descending; ties are broken by `doc_id` ascending.
  - **FR-4.2:** Default Top-K = 10; optional request field `limit` (1..100).
  - **FR-4.3:** `snippet` MUST contain ~200 characters of the original document text around the first occurrence of any query term.

#### UC-5: WebSocket Streaming Search (`/ws/search`)
- **Actor:** Client
- **Description:** Executes the full search pipeline and streams each stage to the client as a separate JSON message.
- **Main flow:**
  1. `stage: "query_expansion"` — the query is enqueued to the Ollama worker; the LLM returns related keywords and synonyms.
  2. `stage: "keyword_search"` — the index is searched with the original + expanded terms.
  3. `stage: "done"` — the pipeline is finished. The connection stays open for further queries.
- **Alternative flows:**
  - **A1 (LLM fallback):** Ollama is unavailable, times out (30 s) or returns an unparsable response → the `query_expansion` message is sent with `"expanded_terms": []` and an extra `"error"` field; the pipeline continues with the original terms only.
  - **A2:** Invalid message (not JSON / empty query) → `{"stage": "error", "data": {"message": "..."}}`; the connection stays open.
- **Requirements:**
  - **FR-5.1:** Each stage MUST be sent immediately when it completes (streaming), not batched at the end.
  - **FR-5.2:** Expanded terms MUST be normalized with the same `Tokenizer`, deduplicated against the original terms and limited to 10 terms.
  - **FR-5.3:** In scoring, original terms have weight 1.0 and expanded terms have weight 0.5.
  - **FR-5.4:** Waiting for the LLM MUST NOT block network threads serving other clients.

### 5.2 Query Expansion (LLM)
- **FR-6.1:** The server calls `POST http://ollama:11434/api/generate` with `{"model": "<OLLAMA_MODEL>", "prompt": "...", "stream": false, "format": "json"}`.
- **FR-6.2:** The prompt asks the model to return a JSON object `{"terms": ["...", ...]}` with up to 10 English keywords and synonyms for the given query.
- **FR-6.3:** The model is pulled automatically on first start (see Section 9).

### 5.3 Clients
- **FR-7.1 (CLI):** `./search "query"` performs a search and prints a numbered list: rank, `doc_id`, score (3 decimals), snippet, followed by total `time_ms`.
- **FR-7.2 (CLI):** Flags: `--mode keyword|stream` (REST or WebSocket pipeline, default `keyword`), `--limit K`, `--host URL` (default `http://localhost:8080`). Non-zero exit code on connection errors.
- **FR-7.3 (Web UI):** A page with a search form, a results list (doc_id, score, snippet) and a pipeline visualization showing the state of each stage (`query_expansion` → `keyword_search` → `done`: pending / running / completed / failed) together with expanded terms and stage timings.
- **FR-7.4 (Web UI):** The Web UI uses the WebSocket endpoint for searches; Nginx proxies `/api/*` and `/ws/*` to the backend so that the UI is served from a single origin.

---

## 6. External Interface Requirements (Fixed Contract)

All bodies are JSON (`Content-Type: application/json`). Error responses have the form `{"error": "<message>"}`.

### 6.1 REST API

| Endpoint | Request | Success response | Error codes |
|---|---|---|---|
| `GET /api/health` | — | `{"status": "ok", "ollama": true\|false}` | — |
| `POST /api/index` | `{"threads": 4}` | `{"status": "ok", "documents_indexed": 2000, "time_ms": 1234}` | 400, 409, 500 |
| `POST /api/search` | `{"query": "text", "mode": "keyword"}` (+ optional `"limit": 10`) | `{"results": [{"doc_id": "...", "score": 0.95, "snippet": "..."}], "time_ms": 56}` | 400 |
| `GET /api/stats` | — | `{"documents": 2000, "index_build_time_ms": 1234, "threads_used": 4}` | — |

### 6.2 WebSocket `/ws/search`
Client request:
```json
{ "query": "search text" }
```
Server messages (one per stage, in this order):
```json
{ "stage": "query_expansion", "data": { "original": "...", "expanded_terms": ["..."] } }
{ "stage": "keyword_search",  "data": { "results": [ {"doc_id": "...", "score": 0.95, "snippet": "..."} ], "time_ms": 12 } }
{ "stage": "done" }
```
Allowed extensions: `"error"` field inside `query_expansion.data` (fallback, UC-5 A1); `{"stage": "error", ...}` message for invalid input (UC-5 A2).

### 6.3 Ports
| Service | Container port | Host port |
|---|---|---|
| `server` | 8080 | 8080 |
| `web` (Nginx) | 80 | 3000 |
| `ollama` | 11434 | not exposed (internal network only) |

---

## 7. Concurrency Model

### 7.1 Concurrent Inverted Index: Striped Locking
The `ConcurrentInvertedIndex` is divided into a fixed number of buckets (default $B = 64$, configurable, power of two). Each bucket contains its own `std::unordered_map<std::string, std::vector<Posting>>` protected by its own `std::shared_mutex`, where `Posting = {uint32_t doc_id; uint32_t tf;}`.
- **Write (indexing):** A worker builds a local `term → tf` map per chunk, accumulates up to 16 chunks into a `MetaBatch` and `BatchPosting` buffer, appends metadata under one `DocumentStore::addBatch` lock, groups batch entries by bucket $i = \text{hash}(term) \bmod B$, and acquires `std::unique_lock` on each affected bucket in ascending order $0 \dots B-1$. At most one bucket lock is held at any time, which rules out deadlocks.
- **Read (searching):** Readers invoke `visitPostings` under a `std::shared_lock` on the buckets of query terms (zero posting-vector copy) and read `DocumentStore` lock-free once `seal()` has run; multiple searches proceed in parallel.
- **Document frequency** is derived from the posting list length; the global document count is a `std::atomic<uint32_t>`.

### 7.2 Thread Pools
1. **Network pool:** A fixed-size `ThreadPool` (size = env `SERVER_THREADS`, default = `std::thread::hardware_concurrency()`, minimum `4` when unknown/1) whose workers run `boost::asio::io_context::run()`. It serves REST and WebSocket sessions concurrently; keyword search is executed on these threads.
2. **Indexer pool:** Created per `POST /api/index` call with exactly $N$ threads ($1 \le N \le 128$); destroyed (joined) after indexing completes.
3. **Ollama worker:** A `ThreadPool` with exactly **1** thread. All query expansion requests are enqueued to it, so LLM requests are processed sequentially (preventing LLM memory exhaustion) while network threads stay unblocked. The WebSocket session continues asynchronously via `net::post(strand, ...)`. During server shutdown, `SearchServer` joins `networkPool` and immediately calls `OllamaService::stop(false)` (cancelling any active `io_context`) while `SearchServer::ioc` and `RequestHandler` remain alive, preventing shutdown Use-After-Free.

### 7.3 Index Snapshot Swap
- The active index is held as `std::atomic<std::shared_ptr<const IndexSnapshot>>` (C++20), where `IndexSnapshot` bundles `ConcurrentInvertedIndex`, `DocumentStore` and `IndexStats`.
- A search loads the current snapshot once and uses it until completion; the old snapshot is freed when the last reader releases it.
- Only one build can run at a time, guarded by `std::atomic<bool> build_in_progress` (`compare_exchange_strong`); a concurrent request gets `409 Conflict`.

### 7.4 Thread-Safety Requirements
- **NFR-C1:** No data races: the full test suite MUST pass under ThreadSanitizer without warnings.
- **NFR-C2:** No deadlocks: at most one index bucket lock is held at a time; no lock is held while waiting on a future or performing network I/O.
- **NFR-C3:** `ThreadPool` destruction MUST finish all queued tasks and join all workers (graceful shutdown).

---

## 8. Data Requirements

### 8.1 Dataset
- **DR-1:** Source: **Project Gutenberg** English plain-text books (`.txt`, UTF-8).
- **DR-2:** Volume: minimum **1 GB** of text and at least **10,000 documents** (target ≥ 2 GB).
- **DR-3:** A Python script `scripts/download_dataset.py` downloads books into `./data/` and strips the Project Gutenberg license header/footer (`*** START OF ... ***` / `*** END OF ... ***`). `IndexerService` also scans `DATASET_PATH` recursively and detects/strips these markers on the fly if raw Gutenberg `.txt` files are placed in subdirectories.
- **DR-4:** The dataset is mounted into the server container read-only: `./data:/data:ro`. The path is configured via env `DATASET_PATH` (default `/data`).

### 8.2 Document Model
- **DR-5:** Each book is split into fixed-size chunks of ~1000 words (on whitespace boundaries). Each chunk is one document.
- **DR-6:** `doc_id = "<book_id>_<chunk_no>"` (e.g. `"1342_17"`), where `book_id` is the Gutenberg ebook number (file name) and `chunk_no` starts from 0.
- **DR-7:** Document text is not kept in RAM; `DocumentStore` keeps `{doc_id, file_path, byte_offset, byte_length, token_count}` so snippets are read from disk on demand.

### 8.3 Text Normalization (Tokenizer)
- **DR-8:** Lowercase ASCII letters; every character that is not `a–z` (punctuation, digits, other symbols) is treated as a separator.
- **DR-9:** Tokens shorter than 2 characters and English stop words (fixed built-in list, ~175 words) are removed.
- **DR-10:** The same tokenizer MUST be used for indexing, REST queries and LLM-expanded terms.

### 8.4 Ranking (Okapi BM25)
For a query with terms $t$ and weights $w_t$ (1.0 original, 0.5 expanded), saturation parameters $k_1 = 1.2$, $b = 0.75$, and average document token count $\text{avgdl}$:

$$
\text{idf}(t) = \ln\left(1 + \frac{\max(0, N - df_t + 0.5)}{df_t + 0.5}\right), \qquad \text{raw}(d) = \sum_{t \in q} w_t \cdot \text{idf}(t) \cdot \frac{tf_{t,d} \cdot (k_1 + 1)}{tf_{t,d} + k_1 \cdot \left(1 - b + b \cdot \frac{|d|}{\text{avgdl}}\right)}
$$

where $|d|$ is the token count of document $d$, $N$ — total documents, $df_t$ — number of documents containing $t$.
- **DR-11:** Scores are normalized to $[0, 1]$ by dividing by the maximum raw score in the result set: $\text{score}(d) = \text{raw}(d) / \max_{d'} \text{raw}(d')$. The top result always has score `1.0`.
- **DR-12:** Matching semantics: OR — a document matches if it contains at least one query term.

---

## 9. Deployment Requirements

- **DEP-1:** `docker compose up` (with no other manual steps except downloading the dataset) MUST start the whole system.
- **DEP-2:** `docker-compose.yml` defines the services:
  | Service | Image / Build | Notes |
  |---|---|---|
  | `server` | Multi-stage build (`gcc` + CMake → slim runtime) | Mounts `./data:/data:ro`; depends on `ollama` |
  | `ollama` | `ollama/ollama` | Named volume `ollama_models` for model cache |
  | `ollama-init` | `ollama/ollama` (one-shot) | Runs `ollama pull qwen2.5:0.5b` against `ollama`, then exits |
  | `web` | `nginx:alpine` | Serves Web UI static files; proxies `/api` and `/ws` to `server:8080` |
- **DEP-3:** The server MUST start and answer `GET /api/health` with 200 even if Ollama is still starting or the model is not yet pulled (`"ollama": false`).
- **DEP-4:** Configuration via environment variables: `PORT` (8080), `SERVER_THREADS`, `DATASET_PATH` (`/data`), `OLLAMA_URL` (`http://ollama:11434`), `OLLAMA_MODEL` (`qwen2.5:0.5b`), `OLLAMA_TIMEOUT_MS` (30000), `INDEX_BUCKETS` (64), `MALLOC_ARENA_MAX` (16).
- **DEP-5:** Docker health checks are defined for `server` (`/api/health`) and `ollama`.

---

## 10. Non-Functional Requirements

### 10.1 Performance and Scalability Research
- **NFR-P1 (Index build):** Measure index build time for `threads` = 1, 2, 4, 8, 16, 32, 64, 128. Each configuration is run 5 times; the median is reported. Report speedup $S(N) = T(1) / T(N)$ and efficiency $E(N) = S(N) / N$.
- **NFR-P2 (Throughput, Load & Spike):** Measure throughput (requests/s) and p95 latency of `POST /api/search` (`mode=keyword`) with 1, 2, 4, 8, 16, 32, 64 concurrent clients (`bench_search.py`), plus sustained load (up to 150 users) and traffic spike bursts (up to 250 users) with Locust (`bench/run_locust_suite.py`).
- **NFR-P3 (Bottleneck analysis):** The report MUST analyze the bottlenecks of parallel processing: disk I/O, lock contention on buckets, thread oversubscription beyond physical cores (32–128 threads), memory allocation (`MALLOC_ARENA_MAX`), the sequential Ollama worker (impact on WebSocket throughput), and Amdahl's law limits.
- **NFR-P4:** Benchmarks are run by reproducible scripts (`bench/bench_index.py`, `bench/bench_search.py`, `bench/run_locust_suite.py`) which output CSV/JSON files and plots (PNG) to `bench/results/`. The hardware (CPU, cores, RAM) is recorded with each run.
- **NFR-P5 (Targets):** With 8 threads on an 8-core machine, the speedup of index build SHOULD be ≥ 4. Keyword search on a built index SHOULD have p95 latency < 100 ms with 1 client.

### 10.2 Reliability
- **NFR-R1:** Invalid client input MUST never crash the server; it results in a 4xx response or a WebSocket `error` stage.
- **NFR-R2:** Unreadable dataset files are skipped and logged; indexing continues.
- **NFR-R3:** Ollama failures MUST be handled by the fallback (UC-5 A1).

### 10.3 Code Quality and Maintainability
- **NFR-Q1:** C++ code follows a unified Microsoft/Allman style (`PascalCase` files/classes, `camelCase` methods/variables, 4-space indentation, Allman braces, max 120 columns) enforced with `clang-format` (`.clang-format` in the repo). Python scripts are syntax-checked in CI and tested via `unittest`.
- **NFR-Q2:** Unit tests cover `ThreadPool`, `Tokenizer`, `ConcurrentInvertedIndex`, `DocumentStore`, BM25 ranking, `RequestHandler`, `OllamaService`, `WebSocketSession` (GoogleTest + ThreadSanitizer), and the Python CLI (`tests/test_cli.py`). Concurrency tests stress writers, readers, pause/resume, concurrent `stop()`, and in-flight shutdown under TSan.
- **NFR-Q3:** CI (GitHub Actions) builds the project, runs tests normally and under ThreadSanitizer, and checks formatting.
- **NFR-Q4:** A search quality table is produced: test query, expected result, actual result (required by the report).

### 10.4 Documentation and Version Control
- **NFR-D1:** `README.md` contains: project description, prerequisites, dataset download, build and run instructions (`docker compose up`), API examples (`curl`), CLI usage, how to run tests and benchmarks.
- **NFR-D2:** UML diagrams: use case, class, sequence (one per use case), deployment (Section 4.2).
- **NFR-D3:** Git history MUST be meaningful and atomic, following Conventional Commits (`feat:`, `fix:`, `docs:`, `test:`, `refactor:`, `chore:`, `perf:`, `build:`, `ci:`).

---

## 11. Traceability Matrix (Grading Criteria → Requirements)

| Grading criterion (points) | Requirements | Verification |
|---|---|---|
| Docker Compose (5) | DEP-1…DEP-5, UC-1 | `docker compose up`; `curl /api/health` → 200 |
| Inverted index (5) | UC-2, UC-3, DR-5…DR-7 | `POST /api/index`, then `GET /api/stats` |
| Keyword search (5) | UC-4, DR-8…DR-12, Section 6.1 | Search quality table, contract tests |
| Thread pool (5) | CON-2, Section 7.2, FR-2.2 | Benchmark NFR-P1 |
| Parallel data structure (5) | CON-3, Section 7.1, NFR-C1, NFR-C2 | Concurrency unit tests under TSan |
| Query expansion (LLM) (5) | Section 5.2, FR-5.2, FR-5.3 | WS search shows `expanded_terms` |
| WebSocket streaming (5) | UC-5, Section 6.2 | Web UI / CLI `--mode stream` |
| CLI client (5) | FR-7.1, FR-7.2 | `./search "query"` |
| Web UI (5) | FR-7.3, FR-7.4 | Manual demo |
| README and clean code (5) | NFR-Q1, NFR-D1, NFR-D3 | Repo review, CI format check |
| Scalability research (10) | NFR-P1…NFR-P4 | Plots and analysis in the report |
| Defense: UML diagrams | NFR-D2 | `docs/` |

---

## 12. Glossary

- **Inverted Index:** A data structure that maps each term to the list of documents (postings) in which it occurs, enabling fast full-text search with an average $O(1)$ term lookup in a hash table.
- **Posting:** An entry in a term's posting list: document id and term frequency in that document.
- **TF-IDF / Okapi BM25:** Ranking schemes that weight rare query terms higher than common terms; Okapi BM25 adds non-linear term-frequency saturation ($k_1$) and document-length normalization ($b$).
- **Query Expansion:** Enriching the user's query with synonyms and related terms (here, generated by an LLM) to increase search recall.
- **Striped Locking:** A concurrency control pattern where a data structure is partitioned into segments (stripes/buckets), each protected by its own independent lock.
- **Reader-Writer Lock (`std::shared_mutex`):** A lock that allows multiple concurrent readers or a single exclusive writer.
- **Thread Pool:** A pattern that maintains a set of worker threads waiting for tasks from a queue, avoiding the overhead of creating and destroying threads for short-lived tasks.
- **Snapshot Swap:** Publishing a fully built immutable data structure by atomically replacing a shared pointer, so readers never observe a partially built state.
- **Speedup / Efficiency:** $S(N) = T(1)/T(N)$; $E(N) = S(N)/N$.
- **Throughput:** Number of requests processed per second.
- **p95 latency:** The latency value below which 95% of requests complete.
- **REST (Representational State Transfer):** An architectural style for stateless request/response APIs over HTTP.
- **WebSocket:** A protocol providing full-duplex communication over a single TCP connection, suited for real-time streaming.
- **Docker Compose:** A tool for defining and running multi-container applications from a single YAML file.