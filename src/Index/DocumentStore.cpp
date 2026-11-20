#include "DocumentStore.h"
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <utility>

uint32_t DocumentStore::add(DocumentMeta meta)
{
    std::unique_lock<std::shared_mutex> lock(mtx);
    const uint32_t internalId = static_cast<uint32_t>(docs.size());
    docs.push_back(std::move(meta));
    return internalId;
}

DocumentMeta DocumentStore::get(uint32_t internalId) const
{
    std::shared_lock<std::shared_mutex> lock(mtx);
    if (internalId >= docs.size())
    {
        throw std::out_of_range("Document ID out of range");
    }
    return docs[internalId];
}

uint32_t DocumentStore::getTokenCount(uint32_t internalId) const
{
    std::shared_lock<std::shared_mutex> lock(mtx);
    if (internalId >= docs.size())
    {
        throw std::out_of_range("Document ID out of range");
    }
    return docs[internalId].tokenCount;
}

std::string DocumentStore::getDocId(uint32_t internalId) const
{
    std::shared_lock<std::shared_mutex> lock(mtx);
    if (internalId >= docs.size())
    {
        throw std::out_of_range("Document ID out of range");
    }
    return docs[internalId].docId;
}

uint32_t DocumentStore::size() const
{
    std::shared_lock<std::shared_mutex> lock(mtx);
    return static_cast<uint32_t>(docs.size());
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
