@startuml
actor Client
participant "WebSocketSession\n(Boost.Asio strand)" as WS
participant "OllamaService" as OllamaSvc
participant "ThreadPool\n(Single-Thread)" as OllamaPool
participant "Ollama API\n(External)" as OllamaAPI
participant "RequestHandler" as Handler
participant "SearchEngine" as Engine
participant "ConcurrentInvertedIndex\n(current snapshot)" as Index
participant "DocumentStore\n(sealed)" as Docs

Client -> WS : WS Upgrade (/ws/search)
Client -> WS : Message {"query": "space movies"}
activate WS

alt invalid JSON or empty query or limit out of [1..100]
    WS -> Client : WS Message {"stage": "error", "data": {"message": ...}}
else valid query
    == Stage 1: Query Expansion ==
    WS -> OllamaSvc : expandQueryAsync(query, callback)
    activate OllamaSvc
    OllamaSvc -> OllamaPool : addTask(performExpansion)
    activate OllamaPool
    OllamaSvc --> WS : return immediately
    deactivate OllamaSvc
    note over WS: Network thread is released;\ncontinuation posted via net::post(strand).
    OllamaPool -> OllamaAPI : POST /api/generate\n{"model": OLLAMA_MODEL, "format": "json", "stream": false}
    activate OllamaAPI
    note right: Sequential 1-thread worker.\nOther WS clients queue cleanly.
    alt Ollama responds in time
        OllamaAPI --> OllamaPool : {"terms": ["sci-fi", "stars", ...]}
        OllamaPool -> OllamaPool : tokenize, dedupe, limit to 10
        OllamaPool -> WS : net::post(strand, onExpansionComplete)
        WS -> Client : WS Message {"stage": "query_expansion",\n"data": {"original": "space movies", "expanded_terms": [...]}}
    else unavailable / timeout (30 s) / unparsable
        OllamaAPI --> OllamaPool : error
        OllamaPool -> WS : net::post(strand, onExpansionComplete + error)
        WS -> Client : WS Message {"stage": "query_expansion",\n"data": {"original": ..., "expanded_terms": [], "error": ...}}
    end
    deactivate OllamaAPI
    deactivate OllamaPool

    == Stage 2: Keyword Search ==
    WS -> Handler : getSnapshot()
    Handler --> WS : shared_ptr<const IndexSnapshot>
    WS -> Engine : buildWeightedTerms(original w=1.0, expanded w=0.5)
    WS -> Engine : search(*snap, weightedTerms, limit)
    activate Engine
    Engine -> Docs : getAverageTokenCount()
    loop For each weighted term
        Engine -> Index : visitPostings(term, visitor)
        activate Index
        note right: shared_lock on bucket;\nzero posting array copy
        Index -> Docs : getTokenCount(docId) (lock-free)
        Index --> Engine : accumulate BM25 in thread_local buffer
        deactivate Index
    end
    Engine -> Engine : normalize to [0..1], partial_sort Top-K
    loop For each Top-K result
        Engine -> Docs : getDocId / readText (from disk)
    end
    Engine --> WS : vector<SearchResult>
    deactivate Engine

    WS -> Client : WS Message {"stage": "keyword_search", "data": {"results": [...], "time_ms": T}}

    == Stage 3: Done ==
    WS -> Client : WS Message {"stage": "done"}
end
deactivate WS
@enduml
