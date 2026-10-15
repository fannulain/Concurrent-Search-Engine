@startuml
actor Client
participant "SearchServer\n(Network Pool thread)" as Server
participant "SearchEngine" as Engine
participant "Tokenizer" as Tok
participant "ConcurrentInvertedIndex\n(current snapshot)" as Index
participant "DocumentStore" as Docs

Client -> Server : POST /api/search {"query": "space movies", "mode": "keyword", "limit": 10}
activate Server

alt empty query / mode != "keyword" / limit not in [1..100]
    Server --> Client : 400 Bad Request {"error": ...}
else valid request
    Server -> Server : snap = snapshot.load()
    alt index not built yet
        Server --> Client : 200 OK {"results": [], "time_ms": 0}
    else
        Server -> Engine : search(snap, query, limit)
        activate Engine
        Engine -> Tok : tokenize(query)
        Tok --> Engine : ["space", "movies"]
        loop For each term
            Engine -> Index : getPostings(term)
            activate Index
            note right: shared_lock on bucket\nhash(term) % 64;\nmany readers in parallel
            Index --> Engine : vector<Posting>
            deactivate Index
        end
        Engine -> Engine : TF-IDF score per document,\nnormalize to [0..1], Top-K
        loop For each Top-K document
            Engine -> Docs : readText(doc_id)
            Docs --> Engine : chunk text (read from disk)
            Engine -> Engine : snippet ~200 chars around first match
        end
        Engine --> Server : vector<SearchResult>
        deactivate Engine
        Server --> Client : 200 OK {"results": [{"doc_id", "score", "snippet"}], "time_ms": T}
    end
end
deactivate Server
note over Client, Docs : LLM is NOT invoked in REST keyword mode
@enduml
