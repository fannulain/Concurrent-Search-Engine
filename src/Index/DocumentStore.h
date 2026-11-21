#ifndef DOCUMENTSTORE_H
#define DOCUMENTSTORE_H

#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <vector>

struct DocumentMeta
{
    std::string docId;
    std::string filePath;
    uint64_t byteOffset = 0;
    uint32_t byteLength = 0;
    uint32_t tokenCount = 0;
};

class DocumentStore
{
private:
    mutable std::shared_mutex mtx;
    std::vector<DocumentMeta> docs;
    uint64_t totalTokens = 0;
    std::atomic<bool> sealed{false};

public:
    DocumentStore() = default;

    DocumentStore(const DocumentStore &) = delete;
    DocumentStore &operator=(const DocumentStore &) = delete;

    uint32_t add(DocumentMeta meta);
    uint32_t addBatch(std::vector<DocumentMeta> &batch);
    void seal();
    bool isSealed() const;

    DocumentMeta get(uint32_t internalId) const;
    uint32_t getTokenCount(uint32_t internalId) const;
    std::string getDocId(uint32_t internalId) const;
    uint32_t size() const;
    double getAverageTokenCount() const;
    std::string readText(uint32_t internalId) const;
};

#endif // DOCUMENTSTORE_H
