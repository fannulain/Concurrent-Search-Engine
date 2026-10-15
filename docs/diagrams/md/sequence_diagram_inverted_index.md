@startuml
actor Admin
participant "SearchServer" as Server
participant "IndexerService" as Indexer
participant "ThreadPool (N threads)" as Pool
participant "Tokenizer" as Tok
participant "DocumentStore" as Docs
participant "ConcurrentInvertedIndex\n(new)" as Index
participant "Bucket [0..63]" as Buckets

Admin -> Server : POST /api/index {"threads": N}
activate Server

alt threads out of range or invalid JSON
    Server --> Admin : 400 Bad Request {"error": ...}
else build_in_progress.compare_exchange(false, true) fails
    Server --> Admin : 409 Conflict {"error": "indexing in progress"}
else
    Server -> Indexer : buildIndex(N)
    activate Indexer

    Indexer -> Index ** : create(64 buckets)
    Indexer -> Docs ** : create()
    Indexer -> Pool ** : create(N)
    Indexer -> Indexer : scanFiles(dataset_path)

    loop For each book file
        Indexer -> Pool : enqueue(index_book_task)
    end

    par N workers in parallel
        loop For each ~1000-word chunk of the book
            Pool -> Tok : tokenize(chunk_text)
            Tok --> Pool : tokens
            Pool -> Pool : build local map term -> tf (no locks)
            Pool -> Docs : add(DocumentMeta)
            note right: short critical section\n(std::mutex)
            Docs --> Pool : internal doc_id
            Pool -> Index : addDocument(doc_id, term_tf)
            activate Index
            loop For each affected bucket i = hash(term) % 64
                Index -> Buckets : unique_lock(shared_mutex[i])
                activate Buckets
                Index -> Buckets : append Posting{doc_id, tf}
                Index -> Buckets : unlock()
                deactivate Buckets
            end
            Index --> Pool : done
            deactivate Index
        end
    end

    Indexer -> Pool : wait all futures
    Indexer -> Pool !! : destroy / join
    Indexer --> Server : shared_ptr<IndexSnapshot> (index, docs, stats)
    deactivate Indexer

    Server -> Server : snapshot.store(new_snapshot)\n(atomic swap)
    Server -> Server : build_in_progress = false
    Server --> Admin : 200 OK {"status": "ok", "documents_indexed": D, "time_ms": T}
end
deactivate Server
@enduml