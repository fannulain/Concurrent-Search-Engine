#include "IndexerService.h"
#include "Containers/DynamicArray.h"
#include "Containers/HashMap.h"
#include "ThreadPool/ThreadPool.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace
{

constexpr size_t CHUNK_WORD_LIMIT = 1000;
constexpr size_t BATCH_CHUNK_LIMIT = 16;
constexpr size_t MAX_HEADER_SCAN_BYTES = 32768;
constexpr size_t MAX_FOOTER_SCAN_BYTES = 65536;

struct FileEntry
{
    std::filesystem::path path;
    uintmax_t size = 0;
};

std::pair<size_t, size_t> computeContentBounds(std::string_view content)
{
    size_t startOffset = 0;
    size_t endOffset = content.size();

    const std::string_view headerSlice = content.substr(0, std::min(content.size(), MAX_HEADER_SCAN_BYTES));
    size_t startMarker = headerSlice.find("*** START OF");
    if (startMarker == std::string_view::npos)
    {
        startMarker = headerSlice.find("***START OF");
    }
    if (startMarker != std::string_view::npos)
    {
        const size_t newlinePos = content.find('\n', startMarker);
        if (newlinePos != std::string_view::npos)
        {
            startOffset = newlinePos + 1;
        }
    }

    const size_t footerSearchBegin =
        content.size() > MAX_FOOTER_SCAN_BYTES ? content.size() - MAX_FOOTER_SCAN_BYTES : 0;
    const std::string_view footerSlice = content.substr(footerSearchBegin);
    size_t endMarkerRel = footerSlice.rfind("*** END OF");
    if (endMarkerRel == std::string_view::npos)
    {
        endMarkerRel = footerSlice.rfind("***END OF");
    }
    if (endMarkerRel != std::string_view::npos)
    {
        const size_t absMarker = footerSearchBegin + endMarkerRel;
        const size_t lineStart = content.rfind('\n', absMarker);
        const size_t candidateEnd = (lineStart == std::string_view::npos) ? absMarker : lineStart + 1;
        if (candidateEnd > startOffset)
        {
            endOffset = candidateEnd;
        }
    }

    if (endOffset <= startOffset)
    {
        return {0, content.size()};
    }
    return {startOffset, endOffset};
}

bool readBinaryFile(const std::filesystem::path &filePath, std::string &outContent)
{
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        return false;
    }

    const std::streamoff fileSize = file.tellg();
    if (fileSize < 0)
    {
        return false;
    }
    if (fileSize == 0)
    {
        outContent.clear();
        return true;
    }

    file.seekg(0, std::ios::beg);
    outContent.resize(static_cast<size_t>(fileSize));
    if (!file.read(outContent.data(), fileSize) && file.gcount() == 0)
    {
        return false;
    }
    outContent.resize(static_cast<size_t>(file.gcount()));
    return true;
}

void indexFile(const std::filesystem::path &filePath, const Tokenizer &tokenizer, IndexSnapshot &snapshot,
               std::mutex &logMtx)
{
    std::string content;
    if (!readBinaryFile(filePath, content))
    {
        std::lock_guard<std::mutex> lock(logMtx);
        std::cerr << "Warning: failed to read dataset file: " << filePath.string() << std::endl;
        return;
    }

    if (content.empty()) return;

    const auto [startOffset, endOffset] = computeContentBounds(content);
    const std::string bookId = filePath.stem().string();
    const std::string filePathStr = filePath.string();

    HashMap<std::string, uint32_t> termFreq;
    termFreq.reserve(512);

    DynamicArray<DocumentMeta> metaBatch;
    metaBatch.reserve(BATCH_CHUNK_LIMIT);
    DynamicArray<BatchPosting> postingBatch;
    postingBatch.reserve(BATCH_CHUNK_LIMIT * 256);

    auto flushBatch = [&]()
    {
        if (metaBatch.empty()) return;
        const uint32_t docCount = static_cast<uint32_t>(metaBatch.size());
        const uint32_t baseId = snapshot.docs.addBatch(metaBatch);
        snapshot.index.addPostingBatch(baseId, docCount, postingBatch);
        metaBatch.clear();
        postingBatch.clear();
    };

    size_t pos = startOffset;
    size_t chunkNo = 0;

    while (pos < endOffset)
    {
        while (pos < endOffset && std::isspace(static_cast<unsigned char>(content[pos])))
        {
            pos++;
        }
        if (pos >= endOffset) break;

        const size_t chunkStart = pos;
        size_t chunkEnd = pos;
        size_t wordCount = 0;

        while (pos < endOffset && wordCount < CHUNK_WORD_LIMIT)
        {
            while (pos < endOffset && !std::isspace(static_cast<unsigned char>(content[pos])))
            {
                pos++;
            }
            chunkEnd = pos;
            wordCount++;

            if (wordCount < CHUNK_WORD_LIMIT)
            {
                while (pos < endOffset && std::isspace(static_cast<unsigned char>(content[pos])))
                {
                    pos++;
                }
            }
        }

        const size_t chunkLen = chunkEnd - chunkStart;
        if (chunkLen == 0) continue;

        const std::string_view chunkView(content.data() + chunkStart, chunkLen);
        termFreq.clear();
        const uint32_t tokenCount = tokenizer.tokenizeToTermFreq(chunkView, termFreq);
        if (tokenCount == 0)
        {
            chunkNo++;
            continue;
        }

        const uint32_t localOffset = static_cast<uint32_t>(metaBatch.size());
        DocumentMeta meta;
        meta.docId = bookId + "_" + std::to_string(chunkNo);
        meta.filePath = filePathStr;
        meta.byteOffset = static_cast<uint64_t>(chunkStart);
        meta.byteLength = static_cast<uint32_t>(chunkLen);
        meta.tokenCount = tokenCount;
        metaBatch.pushBack(std::move(meta));

        for (const auto &[term, tf] : termFreq)
        {
            postingBatch.pushBack(BatchPosting{localOffset, term, tf});
        }

        if (metaBatch.size() >= BATCH_CHUNK_LIMIT)
        {
            flushBatch();
        }
        chunkNo++;
    }

    flushBatch();
}

} // namespace

IndexerService::IndexerService(const Tokenizer &tokenizer, std::string datasetPath, size_t indexBuckets)
    : tokenizer(tokenizer), datasetPath(std::move(datasetPath)), indexBuckets(indexBuckets)
{
}

std::shared_ptr<IndexSnapshot> IndexerService::buildIndex(int threadCount) const
{
    if (threadCount <= 0)
    {
        throw std::invalid_argument("Thread count must be positive");
    }

    const auto startTime = std::chrono::steady_clock::now();

    std::error_code existsEc;
    if (!std::filesystem::exists(datasetPath, existsEc) || !std::filesystem::is_directory(datasetPath, existsEc))
    {
        throw std::runtime_error("Dataset directory does not exist: " + datasetPath);
    }

    DynamicArray<FileEntry> files;
    std::error_code scanEc;
    std::filesystem::recursive_directory_iterator it(
        datasetPath, std::filesystem::directory_options::skip_permission_denied, scanEc);
    std::filesystem::recursive_directory_iterator end;

    for (; it != end; it.increment(scanEc))
    {
        if (scanEc) continue;

        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) continue;

        const std::string ext = it->path().extension().string();
        if (ext == ".txt" || ext == ".TXT")
        {
            std::error_code sizeEc;
            const uintmax_t fileSize = it->file_size(sizeEc);
            files.pushBack(FileEntry{it->path(), sizeEc ? 0 : fileSize});
        }
    }

    if (files.empty())
    {
        throw std::runtime_error("No .txt files found in dataset directory: " + datasetPath);
    }

    std::sort(files.begin(), files.end(),
              [](const FileEntry &a, const FileEntry &b)
              {
                  if (a.size != b.size) return a.size > b.size;
                  return a.path < b.path;
              });

    auto snapshot = std::make_shared<IndexSnapshot>(indexBuckets);
    std::mutex logMtx;

    {
        ThreadPool pool(threadCount);
        DynamicArray<std::future<void>> futures;
        futures.reserve(files.size());

        for (const auto &entry : files)
        {
            const auto &filePath = entry.path;
            futures.pushBack(pool.addTask([filePath, &tokenizer = this->tokenizer, snapshotPtr = snapshot.get(),
                                           &logMtx]() { indexFile(filePath, tokenizer, *snapshotPtr, logMtx); }));
        }

        for (auto &fut : futures)
        {
            fut.get();
        }
        pool.stop(true);
    }

    snapshot->index.shrinkToFit();
    snapshot->docs.seal();

    const auto endTime = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    snapshot->stats.documents = snapshot->docs.size();
    snapshot->stats.buildTimeMs = static_cast<uint64_t>(std::max<long long>(0, elapsedMs));
    snapshot->stats.threadsUsed = static_cast<uint32_t>(threadCount);

    return snapshot;
}
