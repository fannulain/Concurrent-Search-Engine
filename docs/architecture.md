# System Architecture & Implementation Guide

This document describes the internal implementation of the **Concurrent Search Engine with LLM Integration** across all backend, client, and concurrency layers.

---

## 1. High-Level Architecture

The application is composed of four containerized services orchestrated with Docker Compose:

- **`server` (C++20 / Boost.Beast + Boost.Asio):** Executes parallel dataset indexing, serves the REST API (`/api/*`) and WebSocket streaming pipeline (`/ws/search`), and communicates with Ollama.
- **`ollama` + `ollama-init`:** Runs the local LLM runtime and automatically pulls `qwen2.5:0.5b` (configurable via `OLLAMA_MODEL`) on startup.
- **`web` (Nginx):** Serves the static Web UI (`web/static/`) on host port `3000` and reverse-proxies `/api/` and `/ws/` to `server:8080`.
- **`search` (Python CLI):** Standalone console client supporting both REST keyword search (`--mode keyword`) and WebSocket streaming search (`--mode stream`).

---

## 2. Custom Thread Pool & Task Queue (`src/ThreadPool/`)

Implemented from scratch on top of `std::thread`, `std::mutex`, and `std::condition_variable` without third-party thread pools:

- **`TaskQueue<TaskType>` (`TaskQueue.h`):**
  - Thread-safe FIFO queue supporting `push`, `emplace`, `pop`, `clear`, `pause`, `resume`, and `stop(bool terminateInstantly)`.
  - Uses move semantics (`task = std::move(innerQueue.front())`) during `pop`.
  - Unlocks `mtx` prior to calling `condVar.notify_all()` in `stop` and `resume` so woken workers do not contend immediately on the mutex.
- **`ThreadPool` (`ThreadPool.h`, `ThreadPool.cpp`):**
  - `addTask(func, args...)` wraps any callable and arguments in a `std::packaged_task` via `std::apply`, returns `std::future<ReturnType>`, and checks the return value of `queue->push(...)` before incrementing `tasksSubmitted` (eliminating TOCTOU races during shutdown).
  - Exceptions thrown inside tasks propagate cleanly through the returned `std::future` instead of terminating worker threads.
  - Supports graceful shutdown (`stop(true)` in destructor drains all queued tasks and joins every worker thread).

The system uses **three distinct thread pools**:
1. **Network Pool (`SearchServer`):** Fixed pool of `SERVER_THREADS` threads running `boost::asio::io_context::run()` concurrently.
2. **Indexer Pool (`IndexerService`):** Dynamically instantiated per `POST /api/index` call with `N` threads (`1 <= N <= 64`) and joined before publishing the snapshot.
3. **Ollama Worker Pool (`OllamaService`):** Single-thread (`1` worker) queue that serializes LLM `/api/generate` calls to prevent GPU/RAM exhaustion while keeping network I/O threads non-blocking.

---

## 3. Parallel Inverted Index & Snapshot Isolation (`src/Index/`)

### 3.1 `Tokenizer` (`Tokenizer.h`, `Tokenizer.cpp`)
- Converts ASCII letters `A-Z` to lowercase `a-z`.
- Treats every non-letter character (digits, punctuation, whitespace, non-ASCII bytes) as a token separator.
- Filters out tokens shorter than 2 characters and ~175 built-in English stop words stored as `std::string_view` over static literals.
- Exposes both `tokenize()` (queries and LLM output) and `tokenizeToTermFreq()` (zero-intermediate-vector indexing).

### 3.2 `ConcurrentInvertedIndex` (`ConcurrentInvertedIndex.h`, `ConcurrentInvertedIndex.cpp`)
- Partitioned into `B` cache-line-aligned (`alignas(64)`) `Bucket` stripes (`INDEX_BUCKETS`, default `64`).
- Each `Bucket` owns an independent `std::shared_mutex` and `std::unordered_map<std::string, std::vector<Posting>>`, where `Posting = { uint32_t docId; uint32_t tf; }`.
- **Lock-free local aggregation:** Indexing workers build a thread-local `term -> tf` map for each document chunk and group terms by `std::hash<std::string>{}(term) % numBuckets` before touching any lock.
- **Deadlock freedom:** `addDocument` locks at most **one** bucket exclusively (`std::unique_lock`) at a time in index order `0..numBuckets-1`. Readers acquire `std::shared_lock` on only the buckets corresponding to query terms, allowing unrestricted concurrent read parallelism.

### 3.3 `DocumentStore` (`DocumentStore.h`, `DocumentStore.cpp`)
- Protected by `std::shared_mutex`. Stores `DocumentMeta { docId, filePath, byteOffset, byteLength, tokenCount }`.
- Raw document text is **not** kept in RAM; `readText(internalId)` seeks to `byteOffset` in `filePath` and reads `byteLength` bytes on demand for Top-K snippets only.
- `getTokenCount(internalId)` and `getDocId(internalId)` avoid copying `DocumentMeta` strings during TF-IDF scoring loops.

### 3.4 `IndexerService` & Atomic `IndexSnapshot` (`IndexerService.*`, `IndexSnapshot.h`)
- Recursively scans `DATASET_PATH` for `.txt` files, sorting paths deterministically and skipping unreadable files with a warning log.
- Detects optional Project Gutenberg license headers (`*** START OF ... ***`) and footers (`*** END OF ... ***`) to compute exact `[startOffset, endOffset)` byte bounds, then splits books into ~1000-word whitespace-delimited chunks with `docId = "<book_id>_<chunk_no>"`.
- After the indexer `ThreadPool` joins, `RequestHandler` swaps the active index via `std::atomic<std::shared_ptr<const IndexSnapshot>>`. Active search requests keep using their loaded `shared_ptr` without blocking; concurrent rebuild attempts receive `409 Conflict` via `buildInProgress` CAS.

---

## 4. Ranking & Search Pipeline (`src/Search/`, `src/Ollama/`, `src/Server/`)

### 4.1 TF-IDF Scoring (`SearchEngine.h`, `SearchEngine.cpp`)
For query terms $t$ with weight $w_t$ ($1.0$ for original query terms, $0.5$ for deduplicated LLM-expanded terms):

$$\text{raw}(d) = \sum_{t \in q} w_t \cdot \frac{tf_{t,d}}{|d|} \cdot \ln\frac{N}{df_t}$$

Scores are normalized by dividing by $\max_{d'} \text{raw}(d')$ (with a `1.0` fallback if all matched terms have $\ln(N/df_t) = 0$ so `NaN` never occurs). Results are partially sorted with `std::partial_sort` by `score` descending and `docId` ascending for deterministic tie-breaking. Snippets (~200 characters) are extracted around the first case-insensitive match of any query term.

### 4.2 Query Expansion & WebSocket Streaming (`/ws/search`)
1. `ClientSession` upgrades `GET /ws/search` connections to `WebSocketSession` and disables the HTTP idle timer.
2. When `{"query": "..."}` arrives, `WebSocketSession` dispatches query expansion to `OllamaService::expandQueryAsync` (non-blocking) and posts the continuation back onto the session's Boost.Asio strand (`ws.get_executor()`).
3. `OllamaService` posts `{"model": ..., "prompt": ..., "stream": false, "format": "json"}` to Ollama `/api/generate` with `OLLAMA_TIMEOUT_MS` (default 30s). Extracted terms are tokenized, deduplicated against original tokens, and capped at 10 terms. On timeout or network/parse error, an `expanded_terms: []` fallback result with an `error` message is emitted so search still succeeds.
4. Stages are streamed sequentially as separate WebSocket frames as soon as each stage finishes: `query_expansion` $\rightarrow$ `keyword_search` $\rightarrow$ `done`.
