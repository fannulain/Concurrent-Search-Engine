@startuml
skinparam classAttributeIconSize 0

class Config <<struct>> {
  + port: uint16_t
  + serverThreads: int
  + datasetPath: string
  + ollamaHost: string
  + ollamaPort: string
  + ollamaModel: string
  + ollamaTimeoutMs: int
  + indexBuckets: size_t
  + {static} fromEnv(): Config
}

class "TaskQueue<TaskType>" as TaskQueue {
  - innerQueue: std::queue<TaskType>
  - mtx: std::mutex
  - condVar: std::condition_variable
  - stopFlag: bool
  - pauseFlag: bool
  + empty(): bool
  + size(): int
  + push(task): bool
  + pop(task: TaskType&): bool
  + clear(): void
  + stop(terminateInstantly: bool): vector<TaskType>
  + pause(): void
  + resume(): void
}

class ThreadPool {
  - queue: unique_ptr<TaskQueue<function<void()>>>
  - workers: vector<thread>
  - stopFlag: atomic<bool>
  - pauseFlag: atomic<bool>
  - tasksSubmitted: atomic<int>
  - tasksCompleted: atomic<int>
  + ThreadPool(threadCount: int = 0)
  + addTask<Func, Args...>(func, args): future<R>
  + stop(finishTasks: bool = true): void
  + pause(): void
  + resume(): void
  + isWorking(): bool
  + isPaused(): bool
  + getQueueSize(): int
  + getTasksSubmitted(): int
  + getTasksCompleted(): int
  + getThreadCount(): int
  + ~ThreadPool()
}

class Tokenizer {
  - stopWords: unordered_set<string_view>
  + Tokenizer()
  + isStopWord(token: string_view): bool
  + tokenize(text: string_view): vector<string>
  + tokenizeToTermFreq(text: string_view, termFreq: unordered_map<string, uint32_t>&): uint32_t
}

class Posting <<struct>> {
  + docId: uint32_t
  + tf: uint32_t
}

class Bucket <<struct>> {
  + mtx: std::shared_mutex
  + postings: unordered_map<string, vector<Posting>>
}

class ConcurrentInvertedIndex {
  - numBuckets: size_t
  - buckets: vector<unique_ptr<Bucket>>
  - documentCount: atomic<uint32_t>
  + ConcurrentInvertedIndex(bucketCount: size_t = 64)
  + addDocument(docId: uint32_t, termFreq: unordered_map<string, uint32_t>): void
  + getPostings(term: string): vector<Posting>
  + getDocumentCount(): uint32_t
  + getTermCount(): size_t
  + getBucketCount(): size_t
  - getBucketIndex(term: string): size_t
}

class DocumentMeta <<struct>> {
  + docId: string
  + filePath: string
  + byteOffset: uint64_t
  + byteLength: uint32_t
  + tokenCount: uint32_t
}

class DocumentStore {
  - mtx: std::shared_mutex
  - docs: vector<DocumentMeta>
  + add(meta: DocumentMeta): uint32_t
  + get(internalId: uint32_t): DocumentMeta
  + getTokenCount(internalId: uint32_t): uint32_t
  + getDocId(internalId: uint32_t): string
  + size(): uint32_t
  + readText(internalId: uint32_t): string
}

class IndexStats <<struct>> {
  + documents: uint32_t
  + buildTimeMs: uint64_t
  + threadsUsed: uint32_t
}

class IndexSnapshot <<struct>> {
  + index: ConcurrentInvertedIndex
  + docs: DocumentStore
  + stats: IndexStats
}

class WeightedTerm <<struct>> {
  + term: string
  + weight: double
}

class SearchResult <<struct>> {
  + docId: string
  + score: double
  + snippet: string
}

class SearchEngine {
  - tokenizer: const Tokenizer&
  + SearchEngine(tokenizer: const Tokenizer&)
  + buildWeightedTerms(originalQuery: string_view, expandedTerms: vector<string>): vector<WeightedTerm>
  + search(snapshot: const IndexSnapshot&, terms: vector<WeightedTerm>, limit: size_t): vector<SearchResult>
  + searchQuery(snapshot: const IndexSnapshot&, query: string_view, limit: size_t): vector<SearchResult>
  - {static} buildSnippet(text: string_view, terms: vector<WeightedTerm>): string
}

class IndexerService {
  - tokenizer: const Tokenizer&
  - datasetPath: string
  - indexBuckets: size_t
  + IndexerService(tokenizer: const Tokenizer&, datasetPath: string, indexBuckets: size_t = 64)
  + buildIndex(threadCount: int): shared_ptr<IndexSnapshot>
}

class ExpansionResult <<struct>> {
  + originalQuery: string
  + expandedTerms: vector<string>
  + error: string
}

class OllamaService {
  - host: string
  - port: string
  - model: string
  - tokenizer: const Tokenizer&
  - timeoutMs: int
  - workerPool: ThreadPool
  + OllamaService(host, port, model, tokenizer, timeoutMs = 30000)
  + checkHealth(timeout: milliseconds): bool
  + expandQuery(query: string): future<ExpansionResult>
  + expandQueryAsync(query: string, onComplete: function<void(ExpansionResult)>): void
}

class RequestHandler {
  - ollamaService: OllamaService&
  - indexerService: const IndexerService&
  - searchEngine: const SearchEngine&
  - activeSnapshot: atomic<shared_ptr<const IndexSnapshot>>
  - buildInProgress: atomic<bool>
  + handle(request: HttpRequest): HttpResponse
  + getSnapshot(): shared_ptr<const IndexSnapshot>
  + getOllamaService(): OllamaService&
  + getSearchEngine(): const SearchEngine&
}

class ClientSession {
  - stream: beast::tcp_stream
  - requestHandler: RequestHandler&
  + start(): void
}

class WebSocketSession {
  - ws: websocket::stream<beast::tcp_stream>
  - requestHandler: RequestHandler&
  + start(request: HttpRequest): void
}

class SearchServer {
  - port: uint16_t
  - serverThreads: int
  - requestHandler: RequestHandler&
  - ioc: boost::asio::io_context
  - networkPool: unique_ptr<ThreadPool>
  + start(): void
  + stop(): void
}

ThreadPool *-- TaskQueue : owns >
ConcurrentInvertedIndex *-- Bucket : contains >
Bucket o-- Posting : stores >
DocumentStore *-- DocumentMeta : stores >
IndexSnapshot *-- ConcurrentInvertedIndex
IndexSnapshot *-- DocumentStore
IndexSnapshot *-- IndexStats
SearchServer *-- ThreadPool : owns (network pool) >
SearchServer o-- RequestHandler : dispatches to >
SearchServer ..> ClientSession : creates >
ClientSession ..> WebSocketSession : upgrades /ws/search >
ClientSession --> RequestHandler
WebSocketSession --> RequestHandler
RequestHandler o-- IndexSnapshot : atomic shared_ptr >
RequestHandler --> IndexerService
RequestHandler --> SearchEngine
RequestHandler --> OllamaService
IndexerService ..> ThreadPool : creates dynamically (N threads) >
IndexerService ..> IndexSnapshot : builds >
IndexerService ..> Tokenizer : uses >
SearchEngine ..> Tokenizer : uses >
SearchEngine ..> WeightedTerm : uses >
SearchEngine ..> SearchResult : produces >
OllamaService *-- ThreadPool : owns (1 thread) >
OllamaService ..> Tokenizer : normalizes terms >
OllamaService ..> ExpansionResult : produces >
@enduml
