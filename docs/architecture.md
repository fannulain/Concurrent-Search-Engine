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
  - Thread-safe FIFO queue supporting `push`, `emplace`, `pop`, `clear`, `pause`, `resume`, `isPaused`, `getPushedCount`, and `stop(bool terminateInstantly)`.
  - Acts as the single source of truth for both `pauseFlag` and `pushedCount` under `mtx`, preventing TOCTOU pause/resume deadlocks and submission counter inversions.
  - Uses move semantics (`task = std::move(innerQueue.front())`) during `pop` and unlocks `mtx` prior to `condVar.notify_all()` in `stop` and `resume` so woken workers do not contend immediately on the mutex.
- **`ThreadPool` (`ThreadPool.h`, `ThreadPool.cpp`):**
  - `addTask(func, args...)` wraps any callable and arguments in a `std::packaged_task` via `std::apply` and returns `std::future<ReturnType>`.
  - `stop(finishTasks)` serializes concurrent callers under `stopMtx` so every caller waits until all worker threads have actually joined; constructor failure safely cleans up already-spawned threads.
  - Exceptions thrown inside tasks propagate cleanly through the returned `std::future` instead of terminating worker threads.
  - Supports graceful shutdown (`stop(true)` in destructor drains all queued tasks and joins every worker thread).

The system uses **three distinct thread pools**:
1. **Network Pool (`SearchServer`):** Fixed pool of `SERVER_THREADS` threads running `boost::asio::io_context::run()` concurrently (bounded in Docker with `MALLOC_ARENA_MAX=16`).
2. **Indexer Pool (`IndexerService`):** Dynamically instantiated per `POST /api/index` call with `N` threads (`1 <= N <= 128`) and joined before publishing the snapshot.
3. **Ollama Worker Pool (`OllamaService`):** Single-thread (`1` worker) queue that serializes LLM `/api/generate` calls to prevent GPU/RAM exhaustion while keeping network I/O threads non-blocking.

---

## 3. Custom Templated Containers (`src/Containers/`)

To avoid relying on `std::vector` and `std::unordered_map` inside the core indexing and document metadata structures, two generic templated containers are implemented from scratch over raw heap memory:

- **`DynamicArray<T>` (`DynamicArray.h`):**
  - Contiguous resizable array built directly on `::operator new` / `::operator delete` (switching automatically to aligned allocation via `std::align_val_t` when `alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__`).
  - Implements full Rule of Five ownership, `2x` growth, `reserve()`, `shrinkToFit()`, `assign()`, `resize()`, exception-safe element transfer (`noexcept` move preference with copy rollback), self-aliasing-safe `pushBack()` / `emplaceBack()`, and contiguous `T *` / `const T *` iterators compatible with `<algorithm>`.
- **`HashMap<Key, Value, Hash, KeyEqual>` (`HashMap.h`):**
  - Flat open-addressing hash table with linear probing over parallel raw arrays (`SlotState` tags for `Empty` / `Occupied` / `Tombstone` and aligned `Slot` storage accessed via `std::launder`).
  - Applies a SplitMix64 hash finalizer (`mixHash`) before masking against power-of-two capacity, eliminating bucket-bit correlation when all keys inside an index stripe share `hash(term) % numBuckets`.
  - Automatically doubles capacity at 70% occupancy or performs a same-capacity tombstone-purge rehash when tombstones push occupancy below 40% live entries.
  - Exposes `Iterator` / `ConstIterator` (`Entry{key, value}`), `find()`, `contains()`, `at()`, `operator[]`, `tryEmplace()`, `insertOrAssign()`, `erase()`, `reserve()`, and allocation-retaining `clear()`.

---

## 4. Parallel Inverted Index & Snapshot Isolation (`src/Index/`)

### 4.1 `Tokenizer` (`Tokenizer.h`, `Tokenizer.cpp`)
- Converts ASCII letters `A-Z` to lowercase `a-z`.
- Treats every non-letter character (digits, punctuation, whitespace, non-ASCII bytes) as a token separator.
- Filters out tokens shorter than 2 characters and ~175 built-in English stop words stored as `std::string_view` over static literals.
- Exposes both `tokenize()` (queries and LLM output) and `tokenizeToTermFreq()` populating a custom `HashMap<std::string, uint32_t>` (zero-intermediate-vector indexing).

### 4.2 `ConcurrentInvertedIndex` (`ConcurrentInvertedIndex.h`, `ConcurrentInvertedIndex.cpp`)
- Partitioned into `B` cache-line-aligned (`alignas(64)`) `Bucket` stripes (`INDEX_BUCKETS`, default `64`) stored in `DynamicArray<std::unique_ptr<Bucket>>`.
- Each `Bucket` owns an independent `std::shared_mutex` and a custom `HashMap<std::string, DynamicArray<Posting>>`, where `Posting = { uint32_t docId; uint32_t tf; }`.
- **Exception-safe batched aggregation:** Workers group local `term -> tf` entries (`addDocument`) or `BatchPosting` records across up to 16 chunks (`addPostingBatch`) by `std::hash<std::string>{}(term) % numBuckets` using `thread_local DynamicArray<DynamicArray<...>>` buffers guarded by an RAII `GroupedClearGuard`.
- **Deadlock freedom & zero-copy reads:** Writers acquire `std::unique_lock` on at most **one** bucket at a time in ascending order `0..numBuckets-1`. Readers inspect posting arrays in place via `visitPostings()` under `std::shared_lock`, avoiding copies on hot search paths. `shrinkToFit()` trims excess `DynamicArray` capacity after indexing.

### 4.3 `DocumentStore` (`DocumentStore.h`, `DocumentStore.cpp`)
- Stores `DynamicArray<DocumentMeta>` protected by `std::shared_mutex` during construction; supports single `add()` and 16-chunk `addBatch()` inserts with geometric capacity growth while tracking `totalTokens`.
- Once indexing finishes, `seal()` shrinks `docs` (`shrinkToFit()`) and atomically publishes `sealed = true` (`std::memory_order_release`), enabling **lock-free** reads for `get()`, `getTokenCount()`, `getDocId()`, `size()`, and `getAverageTokenCount()` while rejecting subsequent writes with `std::logic_error`.
- Raw document text is **not** kept in RAM; `readText(internalId)` seeks to `byteOffset` in `filePath` and reads `byteLength` bytes on demand for Top-K snippets only.

### 4.4 `IndexerService` & Atomic `IndexSnapshot` (`IndexerService.*`, `IndexSnapshot.h`)
- Recursively scans `DATASET_PATH` for `.txt` files and sorts them descending by file size (Longest-Processing-Time-First / LPT scheduling) to minimize tail stragglers across 8–128 worker threads.
- Detects optional Project Gutenberg license headers (`*** START OF ... ***`) and footers (`*** END OF ... ***`) to compute exact `[startOffset, endOffset)` byte bounds, then splits books into ~1,000-word chunks (`docId = "<book_id>_<chunk_no>"`) flushed in batches of 16 chunks.
- After the indexer `ThreadPool` joins, `index.shrinkToFit()` and `docs.seal()` finalize the snapshot, and `RequestHandler` swaps it via `std::atomic<std::shared_ptr<const IndexSnapshot>>` followed by `malloc_trim(0)`. Active searches retain their loaded `shared_ptr` without blocking; concurrent builds receive `409 Conflict` via `buildInProgress` CAS.

---

## 5. Ranking & Search Pipeline (`src/Search/`, `src/Ollama/`, `src/Server/`)

### 5.1 Okapi BM25 Scoring (`SearchEngine.h`, `SearchEngine.cpp`)
For query terms $t$ with weight $w_t$ ($1.0$ for original query terms, $0.5$ for deduplicated LLM-expanded terms), $k_1 = 1.2$, $b = 0.75$, and $\text{avgdl} = \max(1.0, \text{avgTokenCount})$:

$$\text{idf}(t) = \ln\left(1 + \frac{\max(0, N - df_t + 0.5)}{df_t + 0.5}\right), \qquad \text{raw}(d) = \sum_{t \in q} w_t \cdot \text{idf}(t) \cdot \frac{tf_{t,d} \cdot (k_1 + 1)}{tf_{t,d} + k_1 \cdot \left(1 - b + b \cdot \frac{|d|}{\text{avgdl}}\right)}$$

`search()` accumulates candidate scores in a `thread_local` sparse `scoreBuffer` (reset via RAII guard over `touchedIds`) using zero-copy `visitPostings()` and lock-free `DocumentStore::getTokenCount()`. Scores are normalized by dividing by $\max_{d'} \text{raw}(d')$, partially sorted with `std::partial_sort` (`score` descending, `docId` ascending on ties), and `getDocId()` / `readText()` are resolved only for the final Top-$K$ documents.

### 5.2 Query Expansion, WebSocket Streaming & Clean Shutdown (`/ws/search`)
1. `ClientSession` upgrades `GET /ws/search` connections to `WebSocketSession` and disables the HTTP idle timer.
2. When `{"query": "..."}` arrives, `WebSocketSession` dispatches query expansion to `OllamaService::expandQueryAsync` (non-blocking) and posts the continuation back onto the session's Boost.Asio strand (`ws.get_executor()`).
3. `OllamaService` posts `{"model": ..., "prompt": ..., "stream": false, "format": "json"}` to Ollama `/api/generate` with `OLLAMA_TIMEOUT_MS` (default 30s). Extracted terms are tokenized, deduplicated against original tokens, and capped at 10 terms. On timeout or network/parse error, an `expanded_terms: []` fallback result with an `error` message is emitted so search still succeeds.
4. Stages are streamed sequentially as separate WebSocket frames as soon as each stage finishes: `query_expansion` $\rightarrow$ `keyword_search` $\rightarrow$ `done`.
5. On server shutdown (`SIGINT`/`SIGTERM`), `SearchServer` stops `ioc`, joins `networkPool`, and immediately invokes `OllamaService::stop(false)` (cancelling any active HTTP `io_context`) **before** `SearchServer` and `RequestHandler` are destroyed, preventing Use-After-Free on in-flight callbacks.
