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
  - pushedCount: int
  + empty(): bool
  + size(): int
  + getPushedCount(): int
  + isPaused(): bool
  + push(task): bool
  + emplace(args): bool
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
  - stopMtx: std::mutex
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

class "DynamicArray<T>" as DynamicArray {
  - dataPtr: T*
  - count: size_t
  - cap: size_t
  + pushBack(value): void
  + emplaceBack(args): T&
  + popBack(): void
  + reserve(newCap: size_t): void
  + shrinkToFit(): void
  + resize(newSize: size_t): void
  + assign(newSize: size_t, value): void
  + clear(): void
  + operator[](index: size_t): T&
  + at(index: size_t): T&
  + size(): size_t
  + capacity(): size_t
}

class "HashMap<Key, Value, Hash, KeyEqual>" as HashMap {
  - states: SlotState*
  - slots: Slot*
  - count: size_t
  - tombstones: size_t
  - cap: size_t
  + find(key: Key): Iterator
  + contains(key: Key): bool
  + at(key: Key): Value&
  + operator[](key: Key): Value&
  + tryEmplace(key: Key, args): pair<Iterator, bool>
  + insertOrAssign(key: Key, value: Value): pair<Iterator, bool>
  + erase(key: Key): bool
  + reserve(expectedCount: size_t): void
  + clear(): void
  + size(): size_t
  + capacity(): size_t
  + loadFactor(): double
}

class Tokenizer {
  - stopWords: unordered_set<string_view>
  + Tokenizer()
  + isStopWord(token: string_view): bool
  + tokenize(text: string_view): vector<string>
  + tokenizeToTermFreq(text: string_view, termFreq: HashMap<string, uint32_t>&): uint32_t
}

class Posting <<struct>> {
  + docId: uint32_t
  + tf: uint32_t
}

class BatchPosting <<struct>> {
  + localDocOffset: uint32_t
  + term: string
  + tf: uint32_t
}

class Bucket <<struct>> {
  + mtx: std::shared_mutex
  + postings: HashMap<string, DynamicArray<Posting>>
}

class ConcurrentInvertedIndex {
  - numBuckets: size_t
  - buckets: DynamicArray<unique_ptr<Bucket>>
  - documentCount: atomic<uint32_t>
  + ConcurrentInvertedIndex(bucketCount: size_t = 64)
  + addDocument(docId: uint32_t, termFreq: HashMap<string, uint32_t>): void
  + addPostingBatch(baseDocId: uint32_t, docCount: uint32_t, entries: DynamicArray<BatchPosting>): void
  + getPostings(term: string): DynamicArray<Posting>
  + visitPostings(term: string, visitor: function<void(const DynamicArray<Posting>&)>): void
  + shrinkToFit(): void
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
  - docs: DynamicArray<DocumentMeta>
  - totalTokens: uint64_t
  - sealed: atomic<bool>
  + add(meta: DocumentMeta): uint32_t
  + addBatch(batch: DynamicArray<DocumentMeta>&): uint32_t
  + seal(): void
  + isSealed(): bool
  + get(internalId: uint32_t): DocumentMeta
  + getTokenCount(internalId: uint32_t): uint32_t
  + getDocId(internalId: uint32_t): string
  + size(): uint32_t
  + getAverageTokenCount(): double
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
  - stopping: atomic<bool>
  - activeIocMtx: std::mutex
  - activeIoc: boost::asio::io_context*
  + OllamaService(host, port, model, tokenizer, timeoutMs = 30000)
  + stop(finishTasks: bool = false): void
  + checkHealth(timeout: milliseconds): bool
  + expandQuery(query: string): future<ExpansionResult>
  + expandQueryAsync(query: string, onComplete: function<void(ExpansionResult)>): void
  + getModel(): const string&
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
  - signals: boost::asio::signal_set
  - networkPool: unique_ptr<ThreadPool>
  - isRunning: atomic<bool>
  - boundPort: atomic<uint16_t>
  + start(): void
  + stop(): void
  + waitUntilReady(timeout: milliseconds): bool
  + getBoundPort(): uint16_t
}

ThreadPool *-- TaskQueue : owns >
ConcurrentInvertedIndex *-- Bucket : contains >
ConcurrentInvertedIndex *-- DynamicArray : stores buckets >
ConcurrentInvertedIndex ..> BatchPosting : ingests >
Bucket *-- HashMap : owns >
Bucket *-- DynamicArray : posting lists >
Bucket o-- Posting : stores >
DocumentStore *-- DocumentMeta : stores >
DocumentStore *-- DynamicArray : stores metadata >
Tokenizer ..> HashMap : populates >
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
