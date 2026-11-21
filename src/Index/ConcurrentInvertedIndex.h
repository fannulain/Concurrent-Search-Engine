#ifndef CONCURRENTINVERTEDINDEX_H
#define CONCURRENTINVERTEDINDEX_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct Posting
{
    uint32_t docId = 0;
    uint32_t tf = 0;
};

struct BatchPosting
{
    uint32_t localDocOffset = 0;
    std::string term;
    uint32_t tf = 0;
};

struct alignas(64) Bucket
{
    mutable std::shared_mutex mtx;
    std::unordered_map<std::string, std::vector<Posting>> postings;
};

class ConcurrentInvertedIndex
{
private:
    size_t numBuckets;
    std::vector<std::unique_ptr<Bucket>> buckets;
    std::atomic<uint32_t> documentCount{0};

    size_t getBucketIndex(const std::string &term) const;

public:
    explicit ConcurrentInvertedIndex(size_t bucketCount = 64);

    ConcurrentInvertedIndex(const ConcurrentInvertedIndex &) = delete;
    ConcurrentInvertedIndex &operator=(const ConcurrentInvertedIndex &) = delete;

    void addDocument(uint32_t docId, const std::unordered_map<std::string, uint32_t> &termFreq);
    void addPostingBatch(uint32_t baseDocId, uint32_t docCount, const std::vector<BatchPosting> &entries);
    std::vector<Posting> getPostings(const std::string &term) const;
    void visitPostings(const std::string &term, const std::function<void(const std::vector<Posting> &)> &visitor) const;
    void shrinkToFit();

    uint32_t getDocumentCount() const;
    size_t getTermCount() const;
    size_t getBucketCount() const;
};

#endif // CONCURRENTINVERTEDINDEX_H
