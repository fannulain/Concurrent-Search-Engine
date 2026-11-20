#ifndef INDEXSNAPSHOT_H
#define INDEXSNAPSHOT_H

#include "ConcurrentInvertedIndex.h"
#include "DocumentStore.h"
#include <cstddef>
#include <cstdint>

struct IndexStats
{
    uint32_t documents = 0;
    uint64_t buildTimeMs = 0;
    uint32_t threadsUsed = 0;
};

struct IndexSnapshot
{
    ConcurrentInvertedIndex index;
    DocumentStore docs;
    IndexStats stats;

    explicit IndexSnapshot(size_t bucketCount = 64) : index(bucketCount) {}
};

#endif // INDEXSNAPSHOT_H
