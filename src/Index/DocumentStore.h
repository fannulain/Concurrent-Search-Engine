#ifndef DOCUMENTSTORE_H
#define DOCUMENTSTORE_H

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

public:
    DocumentStore() = default;

    uint32_t add(DocumentMeta meta);
    DocumentMeta get(uint32_t internalId) const;
    uint32_t size() const;
    std::string readText(uint32_t internalId) const;
};

#endif // DOCUMENTSTORE_H
