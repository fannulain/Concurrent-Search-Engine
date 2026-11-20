#ifndef INDEXERSERVICE_H
#define INDEXERSERVICE_H

#include "IndexSnapshot.h"
#include "Tokenizer.h"
#include <cstddef>
#include <memory>
#include <string>

class IndexerService
{
private:
    const Tokenizer& tokenizer;
    std::string datasetPath;
    size_t indexBuckets;

public:
    IndexerService(const Tokenizer& tokenizer, std::string datasetPath, size_t indexBuckets = 64);

    std::shared_ptr<IndexSnapshot> buildIndex(int threadCount) const;
};

#endif // INDEXERSERVICE_H
