@startuml
skinparam classAttributeIconSize 0

class ThreadPool {
  - workers: std::vector<std::thread>
  - tasks: std::queue<std::function<void()>>
  - queue_mutex: std::mutex
  - condition: std::condition_variable
  - stop: bool
  + ThreadPool(threads: size_t)
  + enqueue<F>(task: F): std::future<R>
  + size(): size_t
  + ~ThreadPool()
}

class Tokenizer {
  - stop_words: std::unordered_set<string>
  + tokenize(text: string_view): std::vector<string>
}

class Posting <<struct>> {
  + doc_id: uint32_t
  + tf: uint32_t
}

class Bucket {
  - mutex: std::shared_mutex
  - data: std::unordered_map<string, std::vector<Posting>>
}

class ConcurrentInvertedIndex {
  - num_buckets: size_t
  - buckets: std::vector<Bucket>
  + ConcurrentInvertedIndex(buckets: size_t = 64)
  + addDocument(doc_id: uint32_t, term_tf: map<string, uint32_t>): void
  + getPostings(term: string): std::vector<Posting>
  - getBucketIndex(term: string): size_t
}

class DocumentMeta <<struct>> {
  + doc_id: string
  + file_path: string
  + byte_offset: uint64_t
  + byte_length: uint32_t
  + token_count: uint32_t
}

class DocumentStore {
  - mutex: std::mutex
  - docs: std::vector<DocumentMeta>
  + add(meta: DocumentMeta): uint32_t
  + get(id: uint32_t): const DocumentMeta&
  + size(): uint32_t
  + readText(id: uint32_t): string
}

class IndexStats <<struct>> {
  + documents: uint32_t
  + build_time_ms: uint64_t
  + threads_used: uint32_t
}

class IndexSnapshot {
  + index: ConcurrentInvertedIndex
  + docs: DocumentStore
  + stats: IndexStats
}

class SearchResult <<struct>> {
  + doc_id: string
  + score: double
  + snippet: string
}

class SearchEngine {
  - tokenizer: const Tokenizer&
  + search(snapshot: const IndexSnapshot&, terms: vector<WeightedTerm>, limit: size_t): std::vector<SearchResult>
  - scoreTfIdf(...): double
  - buildSnippet(...): string
}

class IndexerService {
  - tokenizer: const Tokenizer&
  - dataset_path: string
  + buildIndex(threads: size_t): std::shared_ptr<IndexSnapshot>
  - splitIntoChunks(file: string): std::vector<Chunk>
}

class OllamaService {
  - single_thread_worker: ThreadPool
  - host_url: string
  - model: string
  - timeout_ms: int
  + OllamaService(url: string, model: string)
  + checkHealth(): bool
  + expandQuery(query: string): std::future<ExpansionResult>
}

class SearchServer {
  - network_pool: ThreadPool
  - io_context: boost::asio::io_context
  - snapshot: std::atomic<std::shared_ptr<const IndexSnapshot>>
  - build_in_progress: std::atomic<bool>
  - indexer: IndexerService
  - engine: SearchEngine
  - ollama: OllamaService
  + start(port: int): void
  - handleHealth(req, res): void
  - handleIndexPost(req, res): void
  - handleSearchPost(req, res): void
  - handleStats(req, res): void
  - handleWebSocket(ws): void
}

ConcurrentInvertedIndex *-- Bucket : contains >
Bucket o-- Posting : stores >
DocumentStore *-- DocumentMeta : stores >
IndexSnapshot *-- ConcurrentInvertedIndex
IndexSnapshot *-- DocumentStore
IndexSnapshot *-- IndexStats
SearchServer *-- ThreadPool : owns (network pool) >
SearchServer o-- IndexSnapshot : atomic shared_ptr >
SearchServer *-- IndexerService : owns >
SearchServer *-- SearchEngine : owns >
SearchServer *-- OllamaService : owns >
IndexerService ..> ThreadPool : creates dynamically (N threads) >
IndexerService ..> IndexSnapshot : builds >
IndexerService ..> Tokenizer : uses >
SearchEngine ..> Tokenizer : uses >
SearchEngine ..> SearchResult : produces >
OllamaService *-- ThreadPool : owns (1 thread) >
@enduml