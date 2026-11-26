#include "ConcurrentInvertedIndex.h"
#include <functional>
#include <mutex>
#include <utility>

namespace
{

template <typename EntryType> struct GroupedClearGuard
{
    DynamicArray<DynamicArray<EntryType>> &grouped;
    ~GroupedClearGuard()
    {
        for (auto &arr : grouped)
        {
            arr.clear();
        }
    }
};

} // namespace

ConcurrentInvertedIndex::ConcurrentInvertedIndex(size_t bucketCount) : numBuckets(bucketCount == 0 ? 64 : bucketCount)
{
    buckets.reserve(numBuckets);
    for (size_t i = 0; i < numBuckets; i++)
    {
        buckets.pushBack(std::make_unique<Bucket>());
    }
}

size_t ConcurrentInvertedIndex::getBucketIndex(const std::string &term) const
{
    return std::hash<std::string>{}(term) % numBuckets;
}

void ConcurrentInvertedIndex::addDocument(uint32_t docId, const HashMap<std::string, uint32_t> &termFreq)
{
    if (termFreq.empty())
    {
        documentCount.fetch_add(1, std::memory_order_release);
        return;
    }

    using TermEntry = std::pair<const std::string *, uint32_t>;
    thread_local DynamicArray<DynamicArray<TermEntry>> grouped;
    if (grouped.size() != numBuckets)
    {
        grouped.assign(numBuckets, {});
    }
    GroupedClearGuard<TermEntry> clearGuard{grouped};

    for (const auto &[term, tf] : termFreq)
    {
        grouped[getBucketIndex(term)].emplaceBack(&term, tf);
    }

    for (size_t i = 0; i < numBuckets; i++)
    {
        if (grouped[i].empty()) continue;

        std::unique_lock<std::shared_mutex> lock(buckets[i]->mtx);
        auto &bucketPostings = buckets[i]->postings;
        for (const auto &[termPtr, tf] : grouped[i])
        {
            bucketPostings[*termPtr].pushBack(Posting{docId, tf});
        }
        grouped[i].clear();
    }

    documentCount.fetch_add(1, std::memory_order_release);
}

void ConcurrentInvertedIndex::addPostingBatch(uint32_t baseDocId, uint32_t docCount,
                                              const DynamicArray<BatchPosting> &entries)
{
    if (entries.empty())
    {
        if (docCount > 0)
        {
            documentCount.fetch_add(docCount, std::memory_order_release);
        }
        return;
    }

    thread_local DynamicArray<DynamicArray<const BatchPosting *>> grouped;
    if (grouped.size() != numBuckets)
    {
        grouped.assign(numBuckets, {});
    }
    GroupedClearGuard<const BatchPosting *> clearGuard{grouped};

    for (const BatchPosting &entry : entries)
    {
        grouped[getBucketIndex(entry.term)].pushBack(&entry);
    }

    for (size_t i = 0; i < numBuckets; i++)
    {
        if (grouped[i].empty()) continue;

        std::unique_lock<std::shared_mutex> lock(buckets[i]->mtx);
        auto &bucketPostings = buckets[i]->postings;
        for (const BatchPosting *entryPtr : grouped[i])
        {
            bucketPostings[entryPtr->term].pushBack(Posting{baseDocId + entryPtr->localDocOffset, entryPtr->tf});
        }
        grouped[i].clear();
    }

    if (docCount > 0)
    {
        documentCount.fetch_add(docCount, std::memory_order_release);
    }
}

DynamicArray<Posting> ConcurrentInvertedIndex::getPostings(const std::string &term) const
{
    const size_t bucketIndex = getBucketIndex(term);
    std::shared_lock<std::shared_mutex> lock(buckets[bucketIndex]->mtx);

    const auto it = buckets[bucketIndex]->postings.find(term);
    if (it == buckets[bucketIndex]->postings.end())
    {
        return {};
    }
    return it->value;
}

void ConcurrentInvertedIndex::visitPostings(const std::string &term,
                                            const std::function<void(const DynamicArray<Posting> &)> &visitor) const
{
    const size_t bucketIndex = getBucketIndex(term);
    std::shared_lock<std::shared_mutex> lock(buckets[bucketIndex]->mtx);

    const auto it = buckets[bucketIndex]->postings.find(term);
    if (it != buckets[bucketIndex]->postings.end())
    {
        visitor(it->value);
    }
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

void ConcurrentInvertedIndex::shrinkToFit()
{
    for (size_t i = 0; i < numBuckets; i++)
    {
        std::unique_lock<std::shared_mutex> lock(buckets[i]->mtx);
        for (auto &[term, postingList] : buckets[i]->postings)
        {
            (void)term;
            postingList.shrinkToFit();
        }
    }
}
