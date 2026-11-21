@startuml
actor Admin
participant "RequestHandler" as Handler
participant "IndexerService" as Indexer
participant "ThreadPool (N threads)" as Pool
participant "Tokenizer" as Tok
participant "DocumentStore" as Docs
participant "ConcurrentInvertedIndex\n(new)" as Index
participant "Bucket [0..B-1]" as Buckets

Admin -> Handler : POST /api/index {"threads": N} (1..128)
activate Handler

alt threads out of range or invalid JSON
    Handler --> Admin : 400 Bad Request {"error": ...}
else buildInProgress.compare_exchange_strong(false, true) fails
    Handler --> Admin : 409 Conflict {"error": "Indexing is already in progress"}
else
    Handler -> Indexer : buildIndex(N)
    activate Indexer

    Indexer -> Indexer : scanFiles(datasetPath)\n+ LPT sort by file size desc
    Indexer -> Index ** : create(indexBuckets)
    Indexer -> Docs ** : create()
    Indexer -> Pool ** : create(N)

    loop For each book file
        Indexer -> Pool : addTask(indexFile)
    end

    par N workers in parallel
        loop For each ~1000-word chunk (batches of up to 16)
            Pool -> Tok : tokenizeToTermFreq(chunk, termFreq)
            Tok --> Pool : tokenCount
            Pool -> Pool : accumulate metaBatch & postingBatch
        end
        Pool -> Docs : addBatch(metaBatch)
        note right: unique_lock(std::shared_mutex)
        Docs --> Pool : baseDocId
        Pool -> Index : addPostingBatch(baseDocId, docCount, postingBatch)
        activate Index
        loop For each affected bucket i in 0..B-1
            Index -> Buckets : unique_lock(shared_mutex[i])
            activate Buckets
            Index -> Buckets : append Posting{docId, tf}
            Index -> Buckets : unlock()
            deactivate Buckets
        end
        Index --> Pool : done
        deactivate Index
    end

    Indexer -> Pool : wait all futures
    Indexer -> Pool !! : stop(true) / join
    Indexer -> Index : shrinkToFit()
    Indexer -> Docs : seal()
    note right: enables lock-free sealed reads
    Indexer --> Handler : shared_ptr<IndexSnapshot>
    deactivate Indexer

    Handler -> Handler : activeSnapshot.exchange(newSnapshot)\n+ malloc_trim(0)
    Handler -> Handler : buildInProgress = false
    Handler --> Admin : 200 OK {"status": "ok", "documents_indexed": D, "time_ms": T}
end
deactivate Handler
@enduml
