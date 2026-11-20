# Performance & Scalability Benchmark Report

This report documents the empirical performance, scalability, search quality, and parallelism bottleneck analysis of the **Concurrent Search Engine** evaluated against the full English Project Gutenberg dataset (`data/D1.7GB/`).

---

## 1. Test Environment & Dataset

- **CPU:** `13th Gen Intel(R) Core(TM) i5-13450HX` (`16` logical cores)
- **Host OS:** Windows 11 (`Windows-11-10.0.26200-SP0`)
- **Container Runtime:** Docker Engine `28.4.0` (`16` CPUs, `7.57 GiB` VM RAM allocated)
- **Dataset (`data/D1.7GB/`):** `2,808` Project Gutenberg `.txt` files (`1.587 GB` raw text)
- **Indexed Chunks:** `200,001` documents (~1,000 words per chunk after license header/footer stripping)
- **Server Config:** `PORT=8080`, `SERVER_THREADS=16`, `INDEX_BUCKETS=64`, `OLLAMA_MODEL=qwen2.5:0.5b`

---

## 2. Parallel Index Build Scalability (`POST /api/index`)

Measured across `threads` = `1, 2, 4, 8, 16` with 5 independent runs per configuration via `bench/bench_index.py`:

| Threads ($N$) | Documents | Median Time (ms) | Min (ms) | Max (ms) | Speedup $S(N)$ | Efficiency $E(N)$ |
|---|---|---|---|---|---|---|
| 1 | 200,001 | 129,013 | 125,977 | 137,158 | 1.000x | 100.0% |
| 2 | 200,001 | 64,315 | 60,606 | 67,246 | 2.006x | 100.3% |
| 4 | 200,001 | 36,409 | 35,315 | 36,736 | 3.543x | 88.6% |
| 8 | 200,001 | 23,503 | 22,598 | 26,242 | 5.489x | 68.6% |
| 16 | 200,001 | 21,674 | 19,517 | 23,081 | 5.952x | 37.2% |

### Observations
- **Target met (SRS NFR-P5):** At $N = 8$ threads, median build time drops from `129.0 s` to `23.5 s`, achieving **$S(8) = 5.49\times$** speedup (exceeding the $\ge 4\times$ requirement).
- **Near-linear scaling up to 4 threads:** $S(2) = 2.01\times$ ($100.3\%$ efficiency) and $S(4) = 3.54\times$ ($88.6\%$ efficiency), confirming that thread-local tokenization (`termFreq` reuse) and cache-aligned 64-bucket striped locking minimize write contention.
- **Diminishing returns from 8 to 16 threads:** Speedup rises modestly from $5.49\times$ to $5.95\times$ ($37.2\%$ efficiency at $N=16$), reflecting hardware SMT/core saturation, allocator contention, and serial phases.
- **Memory footprint:** After shrinking posting vector capacities (`shrinkToFit()`) and returning freed glibc arenas (`malloc_trim(0)`), steady-state `server` RSS stayed at `3.017 GiB / 7.569 GiB` (`39.86%`).

---

## 3. Concurrent Keyword Search Scalability (`POST /api/search`)

Measured across `1, 2, 4, 8, 16, 32, 64` concurrent keep-alive clients for `30 s` per concurrency level via `bench/bench_search.py`:

| Concurrent Clients | Completed Requests | Errors | Throughput (req/s) | Avg Latency (ms) | p50 (ms) | p95 (ms) | p99 (ms) |
|---|---|---|---|---|---|---|---|
| 1 | 467 | 0 | 15.55 | 64.30 | 60.75 | 105.58 | 161.42 |
| 2 | 967 | 0 | 32.19 | 62.11 | 58.12 | 98.04 | 201.32 |
| 4 | 2,031 | 0 | 67.62 | 59.12 | 57.28 | 86.41 | 107.19 |
| 8 | 3,669 | 0 | 122.06 | 65.47 | 62.08 | 99.20 | 114.69 |
| 16 | 4,320 | 0 | 143.42 | 111.34 | 107.72 | 176.65 | 200.84 |
| 32 | 4,323 | 0 | 143.14 | 222.72 | 215.38 | 311.19 | 471.65 |
| 64 | 4,496 | 0 | 147.44 | 430.12 | 365.30 | 695.28 | 759.43 |

### Observations
- **Zero failures:** All `20,273` requests across all 7 concurrency levels succeeded (`0` errors).
- **Linear throughput scaling up to 8 clients:** Throughput climbs from `15.55 req/s` (1 client) to `122.06 req/s` (8 clients — $7.85\times$ scaling), while p50 latency remains flat (`57–62 ms`) and p95 stays around `86–105 ms`.
- **Saturation plateau at 16–64 clients:** Throughput peaks around `143–147 req/s` once all `16` server network threads and CPU execution resources are saturated. Beyond 16 clients, requests queue cleanly at the Boost.Asio acceptor/socket layer, keeping throughput stable while latency grows proportionally with queue depth.

---

## 4. Search Quality Evaluation

Evaluated against the `200,001`-chunk corpus via `POST /api/search` (`mode="keyword"`, `limit=5`):

| Test Query | Expected Result | Actual Top Result | Score | Top-3 Doc IDs | Server Time (ms) | Status |
|---|---|---|---|---|---|---|
| `divine comedy inferno` | Dante's Divine Comedy / Inferno chunks | `2620_182` | 1.000 | `2620_182, 1003_40, 1433_13` | 44 | PASS |
| `sherlock holmes detective` | Arthur Conan Doyle Sherlock Holmes stories | `108_112` | 1.000 | `108_112, 446_64, 446_69` | 39 | PASS |
| `french revolution guillotine` | Historical or literary texts on the French Revolution | `1276_19` | 1.000 | `1276_19, 448_2, 1359_15` | 47 | PASS |
| `genesis creation light earth` | King James Bible / Genesis passages | `1611_47` | 1.000 | `1611_47, 618_0, 3458_150` | 66 | PASS |
| `alice wonderland white rabbit` | Lewis Carroll's Alice in Wonderland | `114_0` | 1.000 | `114_0, 688_26, 11_16` | 54 | PASS |
| `xqzvnonexistentkplm` | Empty result set (no matches) | `(empty)` | — | `—` | 0 | PASS |
| `the and of in` | Empty result set (stop words only) | `(empty)` | — | `—` | 0 | PASS |

CLI live tests on `"divine comedy inferno"` additionally verified end-to-end WebSocket streaming (`--mode stream`):
- Stage `query_expansion`: `expanded_terms=['cosmic', 'world', 'infinite', 'play', 'fantasy', 'drama']`
- Stage `keyword_search`: 5 ranked results returned in `80 ms` (vs `47 ms` in REST keyword mode)
- Stage `done`: completed cleanly

---

## 5. Parallelism Bottleneck Analysis

1. **Amdahl's Law Serial Fraction:**
   Using $S(8) = 5.489$ and $S(16) = 5.952$, Amdahl's law $S(N) = \frac{1}{s + (1 - s)/N}$ yields an effective serial/contention fraction of roughly $s \approx 6.5\%–11\%$. Serial components include recursive directory traversal and sorting, thread pool creation/teardown, and the post-indexing `shrinkToFit()` + `malloc_trim(0)` pass.
2. **Disk I/O & Container Bind-Mount Overhead:**
   Reading `2,808` files totaling `1.587 GB` from a Windows host bind mount (`./data:/data:ro`) into Docker Desktop WSL2 traverses cross-VM filesystem translation (9p/drvfs), though repeated runs benefit heavily from the Linux VM page cache. During search, Top-$K$ snippet generation performs $K$ random file seeks on disk.
3. **Bucket & Registry Lock Contention (`std::shared_mutex`):**
   - *Indexing:* Each of the `200,001` chunks acquires a short `unique_lock` on `DocumentStore::mtx` and up to `64` bucket `unique_lock`s in `ConcurrentInvertedIndex`. Under `16` concurrent writers, shared bucket hash collisions (especially for high-frequency terms) and `DocumentStore::mtx` serialization limit scaling beyond 8 threads.
   - *Searching:* Even though `getPostings()` and `getTokenCount()` use `std::shared_lock`, `std::shared_mutex` updates an atomic reader counter on every lock/unlock. Across `16–64` concurrent clients executing queries that touch tens of thousands of postings, cache-line bouncing on `DocumentStore::mtx` (called once per posting to retrieve `|d|`) caps single-node throughput around `~145 req/s`. Storing `tokenCount` in a lock-free `const` vector on immutable snapshots or inside `Posting` would raise read throughput substantially.
4. **Memory Allocation & Heap Fragmentation:**
   Inserting millions of postings into `std::unordered_map<std::string, std::vector<Posting>>` causes frequent dynamic allocations and vector capacity doublings across per-thread `glibc` malloc arenas. Reusing `thread_local` bucket buffers, reserving `termFreq`, calling `shrinkToFit()`, and triggering `malloc_trim(0)` reduced RSS dramatically (`3.02 GiB` steady state), though allocator synchronization still contributes overhead at high thread counts.
5. **Sequential Ollama LLM Worker Impact on WebSocket Throughput:**
   `OllamaService` intentionally routes all `/api/generate` calls through a **1-thread `ThreadPool`** to avoid exhausting local CPU/RAM with parallel LLM inference. While `WebSocketSession` releases network threads immediately (keeping REST keyword traffic and other WebSocket clients responsive), total WebSocket streaming throughput is strictly bounded by sequential LLM latency ($\sim 1\text{–}3\text{ s}$ per query depending on hardware), whereas REST keyword search sustains `~145 req/s`.
