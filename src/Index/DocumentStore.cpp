#include "DocumentStore.h"
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <utility>

uint32_t DocumentStore::add(DocumentMeta meta)
{
    std::unique_lock<std::shared_mutex> lock(mtx);
    if (sealed.load(std::memory_order_acquire))
    {
        throw std::logic_error("DocumentStore is sealed");
    }
    const uint32_t internalId = static_cast<uint32_t>(docs.size());
    totalTokens += meta.tokenCount;
    docs.pushBack(std::move(meta));
    return internalId;
}

uint32_t DocumentStore::addBatch(DynamicArray<DocumentMeta> &batch)
{
    if (batch.empty())
    {
        return size();
    }

    std::unique_lock<std::shared_mutex> lock(mtx);
    if (sealed.load(std::memory_order_acquire))
    {
        throw std::logic_error("DocumentStore is sealed");
    }

    const uint32_t baseId = static_cast<uint32_t>(docs.size());
    docs.reserve(docs.size() + batch.size());
    for (DocumentMeta &meta : batch)
    {
        const uint32_t tok = meta.tokenCount;
        docs.pushBack(std::move(meta));
        totalTokens += tok;
    }
    return baseId;
}

void DocumentStore::seal()
{
    std::unique_lock<std::shared_mutex> lock(mtx);
    docs.shrinkToFit();
    sealed.store(true, std::memory_order_release);
}

bool DocumentStore::isSealed() const
{
    return sealed.load(std::memory_order_acquire);
}

DocumentMeta DocumentStore::get(uint32_t internalId) const
{
    if (sealed.load(std::memory_order_acquire))
    {
        if (internalId >= docs.size())
        {
            throw std::out_of_range("Document ID out of range");
        }
        return docs[internalId];
    }

    std::shared_lock<std::shared_mutex> lock(mtx);
    if (internalId >= docs.size())
    {
        throw std::out_of_range("Document ID out of range");
    }
    return docs[internalId];
}

uint32_t DocumentStore::getTokenCount(uint32_t internalId) const
{
    if (sealed.load(std::memory_order_acquire))
    {
        if (internalId >= docs.size())
        {
            throw std::out_of_range("Document ID out of range");
        }
        return docs[internalId].tokenCount;
    }

    std::shared_lock<std::shared_mutex> lock(mtx);
    if (internalId >= docs.size())
    {
        throw std::out_of_range("Document ID out of range");
    }
    return docs[internalId].tokenCount;
}

std::string DocumentStore::getDocId(uint32_t internalId) const
{
    if (sealed.load(std::memory_order_acquire))
    {
        if (internalId >= docs.size())
        {
            throw std::out_of_range("Document ID out of range");
        }
        return docs[internalId].docId;
    }

    std::shared_lock<std::shared_mutex> lock(mtx);
    if (internalId >= docs.size())
    {
        throw std::out_of_range("Document ID out of range");
    }
    return docs[internalId].docId;
}

uint32_t DocumentStore::size() const
{
    if (sealed.load(std::memory_order_acquire))
    {
        return static_cast<uint32_t>(docs.size());
    }

    std::shared_lock<std::shared_mutex> lock(mtx);
    return static_cast<uint32_t>(docs.size());
}

double DocumentStore::getAverageTokenCount() const
{
    if (sealed.load(std::memory_order_acquire))
    {
        return docs.empty() ? 0.0 : static_cast<double>(totalTokens) / static_cast<double>(docs.size());
    }

    std::shared_lock<std::shared_mutex> lock(mtx);
    return docs.empty() ? 0.0 : static_cast<double>(totalTokens) / static_cast<double>(docs.size());
}

std::string DocumentStore::readText(uint32_t internalId) const
{
    const DocumentMeta meta = get(internalId);
    if (meta.byteLength == 0)
    {
        return {};
    }

    std::ifstream file(meta.filePath, std::ios::binary);
    if (!file.is_open())
    {
        return {};
    }

    file.seekg(static_cast<std::streamoff>(meta.byteOffset), std::ios::beg);
    if (!file.good())
    {
        return {};
    }

    std::string text(meta.byteLength, '\0');
    file.read(text.data(), static_cast<std::streamsize>(meta.byteLength));
    const std::streamsize bytesRead = file.gcount();
    if (bytesRead <= 0)
    {
        return {};
    }

    text.resize(static_cast<size_t>(bytesRead));
    return text;
}
