#include "ConcurrentInvertedIndex.h"
#include <functional>
#include <mutex>
#include <utility>

ConcurrentInvertedIndex::ConcurrentInvertedIndex(size_t bucketCount) : numBuckets(bucketCount == 0 ? 64 : bucketCount)
{
    buckets.reserve(numBuckets);
    for (size_t i = 0; i < numBuckets; i++)
    {
        buckets.push_back(std::make_unique<Bucket>());
    }
}

size_t ConcurrentInvertedIndex::getBucketIndex(const std::string &term) const
{
    return std::hash<std::string>{}(term) % numBuckets;
}

void ConcurrentInvertedIndex::addDocument(uint32_t docId, const std::unordered_map<std::string, uint32_t> &termFreq)
{
    if (termFreq.empty())
    {
        documentCount.fetch_add(1, std::memory_order_release);
        return;
    }

    using TermEntry = std::pair<const std::string *, uint32_t>;
    std::vector<std::vector<TermEntry>> grouped(numBuckets);

    for (const auto &[term, tf] : termFreq)
    {
        grouped[getBucketIndex(term)].emplace_back(&term, tf);
    }

    // lock only one bucket at a time to prevent deadlocks across workers
    for (size_t i = 0; i < numBuckets; i++)
    {
        if (grouped[i].empty()) continue;

        std::unique_lock<std::shared_mutex> lock(buckets[i]->mtx);
        auto &bucketPostings = buckets[i]->postings;
        for (const auto &[termPtr, tf] : grouped[i])
        {
            bucketPostings[*termPtr].push_back(Posting{docId, tf});
        }
    }

    documentCount.fetch_add(1, std::memory_order_release);
}

std::vector<Posting> ConcurrentInvertedIndex::getPostings(const std::string &term) const
{
    const size_t bucketIndex = getBucketIndex(term);
    std::shared_lock<std::shared_mutex> lock(buckets[bucketIndex]->mtx);

    const auto it = buckets[bucketIndex]->postings.find(term);
    if (it == buckets[bucketIndex]->postings.end())
    {
        return {};
    }
    return it->second;
}

uint32_t ConcurrentInvertedIndex::getDocumentCount() const
{
    return documentCount.load(std::memory_order_acquire);
}

size_t ConcurrentInvertedIndex::getTermCount() const
{
    size_t totalTerms = 0;
    for (size_t i = 0; i < numBuckets; i++)
    {
        std::shared_lock<std::shared_mutex> lock(buckets[i]->mtx);
        totalTerms += buckets[i]->postings.size();
    }
    return totalTerms;
}

size_t ConcurrentInvertedIndex::getBucketCount() const
{
    return numBuckets;
}
