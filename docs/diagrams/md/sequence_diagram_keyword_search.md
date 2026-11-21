@startuml
actor Client
participant "RequestHandler\n(Network Pool thread)" as Handler
participant "SearchEngine" as Engine
participant "Tokenizer" as Tok
participant "ConcurrentInvertedIndex\n(current snapshot)" as Index
participant "DocumentStore\n(sealed)" as Docs

Client -> Handler : POST /api/search {"query": "space movies", "mode": "keyword", "limit": 10}
activate Handler

alt empty query / mode != "keyword" / limit not in [1..100]
    Handler --> Client : 400 Bad Request {"error": ...}
else valid request
    Handler -> Handler : snap = activeSnapshot.load()
    alt index not built yet
        Handler --> Client : 200 OK {"results": [], "time_ms": 0}
    else
        Handler -> Engine : searchQuery(*snap, query, limit)
        activate Engine
        Engine -> Tok : tokenize(query)
        Tok --> Engine : ["space", "movies"]
        Engine -> Docs : getAverageTokenCount()
        loop For each term
            Engine -> Index : visitPostings(term, visitor)
            activate Index
            note right: shared_lock on bucket\nhash(term) % B;\nmany readers in parallel
            Index -> Docs : getTokenCount(docId) (lock-free)
            Index --> Engine : accumulate BM25 scores
            deactivate Index
        end
        Engine -> Engine : normalize to [0..1], partial_sort Top-K
        loop For each Top-K document
            Engine -> Docs : getDocId / readText(internalId)
            Docs --> Engine : chunk text (read from disk)
            Engine -> Engine : snippet ~200 chars around first match
        end
        Engine --> Handler : vector<SearchResult>
        deactivate Engine
        Handler --> Client : 200 OK {"results": [{"doc_id", "score", "snippet"}], "time_ms": T}
    end
end
deactivate Handler
note over Client, Docs : LLM is NOT invoked in REST keyword mode
@enduml
