@startuml
actor Client
participant "SearchServer\n(WebSocket Handler)" as Server
participant "OllamaService" as OllamaSvc
participant "ThreadPool\n(Single-Thread)" as OllamaPool
participant "Ollama API\n(External)" as OllamaAPI
participant "SearchEngine" as Engine
participant "ConcurrentInvertedIndex\n(current snapshot)" as Index

Client -> Server : WS Connect (/ws/search)
Client -> Server : Message {"query": "space movies"}
activate Server

alt invalid JSON or empty query
    Server -> Client : WS Message {"stage": "error", "data": {"message": ...}}
else valid query
    == Stage 1: Query Expansion ==
    Server -> OllamaSvc : expandQuery("space movies")
    activate OllamaSvc
    OllamaSvc -> OllamaPool : enqueue(LLM request task)
    activate OllamaPool
    OllamaSvc --> Server : std::future<ExpansionResult>
    deactivate OllamaSvc
    note over Server: Network thread is released;\nthe session resumes when the future is ready.
    OllamaPool -> OllamaAPI : POST /api/generate\n{"model": "qwen2.5:0.5b", "format": "json", "stream": false}
    activate OllamaAPI
    note right: Waiting for generation.\nOther WS clients are queued in the pool.
    alt Ollama responds in time
        OllamaAPI --> OllamaPool : {"terms": ["sci-fi", "stars", ...]}
        OllamaPool -> OllamaPool : tokenize, dedupe, limit to 10
        OllamaPool --> Server : expanded_terms
        Server -> Client : WS Message {"stage": "query_expansion",\n"data": {"original": "space movies", "expanded_terms": [...]}}
    else unavailable / timeout (30 s) / unparsable
        OllamaAPI --> OllamaPool : error
        OllamaPool --> Server : empty result + error
        Server -> Client : WS Message {"stage": "query_expansion",\n"data": {"original": ..., "expanded_terms": [], "error": ...}}
    end
    deactivate OllamaAPI
    deactivate OllamaPool

    == Stage 2: Keyword Search ==
    Server -> Server : snap = snapshot.load()
    Server -> Engine : search(snap, original (w=1.0) + expanded (w=0.5), 10)
    activate Engine
    Engine -> Index : getPostings(term) for each term
    activate Index
    note right: Reading buckets via\nshared_lock (shared_mutex)
    Index --> Engine : posting lists
    deactivate Index
    Engine -> Engine : TF-IDF, normalize to [0..1], Top-K, snippets
    Engine --> Server : vector<SearchResult>
    deactivate Engine

    Server -> Client : WS Message {"stage": "keyword_search", "data": {"results": [...], "time_ms": 12}}

    == Stage 3: Done ==
    Server -> Client : WS Message {"stage": "done"}
end
deactivate Server
@enduml