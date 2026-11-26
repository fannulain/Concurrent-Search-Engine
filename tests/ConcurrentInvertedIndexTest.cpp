#include "Index/ConcurrentInvertedIndex.h"
#include "Containers/DynamicArray.h"
#include "Containers/HashMap.h"
#include "Index/DocumentStore.h"
#include <atomic>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

TEST(ConcurrentInvertedIndexTest, AddsAndRetrievesPostings)
{
    ConcurrentInvertedIndex index(16);

    index.addDocument(0, {{"alpha", 2}, {"beta", 1}});
    index.addDocument(1, {{"alpha", 5}, {"gamma", 3}});

    EXPECT_EQ(index.getDocumentCount(), 2u);
    EXPECT_EQ(index.getTermCount(), 3u);
    EXPECT_EQ(index.getBucketCount(), 16u);

    const DynamicArray<Posting> alphaPostings = index.getPostings("alpha");
    ASSERT_EQ(alphaPostings.size(), 2u);
    EXPECT_EQ(alphaPostings[0].docId, 0u);
    EXPECT_EQ(alphaPostings[0].tf, 2u);
    EXPECT_EQ(alphaPostings[1].docId, 1u);
    EXPECT_EQ(alphaPostings[1].tf, 5u);

    EXPECT_TRUE(index.getPostings("missing").empty());
}

TEST(ConcurrentInvertedIndexTest, ConcurrentWritersAndReadersUnderTsan)
{
    ConcurrentInvertedIndex index(64);
    constexpr int WRITER_THREADS = 8;
    constexpr int DOCS_PER_WRITER = 100;
    constexpr int READER_THREADS = 8;

    std::atomic<bool> writersDone{false};
    std::vector<std::thread> writers;
    std::vector<std::thread> readers;

    for (int w = 0; w < WRITER_THREADS; w++)
    {
        writers.emplace_back(
            [&index, w]()
            {
                for (int d = 0; d < DOCS_PER_WRITER; d++)
                {
                    const uint32_t docId = static_cast<uint32_t>(w * DOCS_PER_WRITER + d);
                    HashMap<std::string, uint32_t> tf;
                    tf["shared"] = 1;
                    tf["writer_" + std::to_string(w)] = 2;
                    tf["term_" + std::to_string(d % 20)] = 3;
                    index.addDocument(docId, tf);
                }
            });
    }

    for (int r = 0; r < READER_THREADS; r++)
    {
        readers.emplace_back(
            [&index, &writersDone, r]()
            {
                while (!writersDone.load(std::memory_order_acquire))
                {
                    const auto sharedPostings = index.getPostings("shared");
                    const auto writerPostings = index.getPostings("writer_" + std::to_string(r));
                    EXPECT_LE(sharedPostings.size(), static_cast<size_t>(WRITER_THREADS * DOCS_PER_WRITER));
                    EXPECT_LE(writerPostings.size(), static_cast<size_t>(DOCS_PER_WRITER));
                }
            });
    }

    for (auto &writer : writers)
    {
        writer.join();
    }
    writersDone.store(true, std::memory_order_release);

    for (auto &reader : readers)
    {
        reader.join();
    }

    EXPECT_EQ(index.getDocumentCount(), static_cast<uint32_t>(WRITER_THREADS * DOCS_PER_WRITER));
    EXPECT_EQ(index.getPostings("shared").size(), static_cast<size_t>(WRITER_THREADS * DOCS_PER_WRITER));
    for (int w = 0; w < WRITER_THREADS; w++)
    {
        EXPECT_EQ(index.getPostings("writer_" + std::to_string(w)).size(), static_cast<size_t>(DOCS_PER_WRITER));
    }
}

TEST(ConcurrentInvertedIndexTest, SupportsBatchedWritesAndVisitPostings)
{
    ConcurrentInvertedIndex index(16);
    DocumentStore docs;

    DynamicArray<DocumentMeta> batch1 = {
        DocumentMeta{"doc_0", "a.txt", 0, 10, 10},
        DocumentMeta{"doc_1", "a.txt", 10, 10, 20},
    };
    const uint32_t base1 = docs.addBatch(batch1);
    EXPECT_EQ(base1, 0u);

    index.addPostingBatch(base1, 2,
                          {
                              BatchPosting{0, "alpha", 3},
                              BatchPosting{0, "beta", 1},
                              BatchPosting{1, "alpha", 2},
                          });

    DynamicArray<DocumentMeta> batch2 = {
        DocumentMeta{"doc_2", "b.txt", 0, 15, 30},
    };
    const uint32_t base2 = docs.addBatch(batch2);
    EXPECT_EQ(base2, 2u);
    index.addPostingBatch(base2, 1, {BatchPosting{0, "gamma", 4}});

    EXPECT_EQ(index.getDocumentCount(), 3u);
    EXPECT_EQ(docs.size(), 3u);
    EXPECT_DOUBLE_EQ(docs.getAverageTokenCount(), 20.0);

    size_t visitedCount = 0;
    index.visitPostings("alpha",
                        [&visitedCount](const DynamicArray<Posting> &postings)
                        {
                            visitedCount = postings.size();
                            ASSERT_EQ(postings.size(), 2u);
                            EXPECT_EQ(postings[0].docId, 0u);
                            EXPECT_EQ(postings[0].tf, 3u);
                            EXPECT_EQ(postings[1].docId, 1u);
                            EXPECT_EQ(postings[1].tf, 2u);
                        });
    EXPECT_EQ(visitedCount, 2u);

    bool calledMissing = false;
    index.visitPostings("missing", [&calledMissing](const DynamicArray<Posting> &) { calledMissing = true; });
    EXPECT_FALSE(calledMissing);
}

TEST(ConcurrentInvertedIndexTest, DocumentStoreSealBlocksMutationsAndAllowsConcurrentReads)
{
    DocumentStore docs;
    DynamicArray<DocumentMeta> batch;
    for (int i = 0; i < 50; i++)
    {
        batch.pushBack(DocumentMeta{"doc_" + std::to_string(i), "file.txt", 0, 100, static_cast<uint32_t>(10 + i)});
    }
    docs.addBatch(batch);
    docs.seal();

    EXPECT_TRUE(docs.isSealed());
    EXPECT_THROW(docs.add(DocumentMeta{"late", "file.txt", 0, 1, 1}), std::logic_error);
    DynamicArray<DocumentMeta> lateBatch = {DocumentMeta{"late2", "file.txt", 0, 1, 1}};
    EXPECT_THROW(docs.addBatch(lateBatch), std::logic_error);

    std::vector<std::thread> readers;
    for (int r = 0; r < 8; r++)
    {
        readers.emplace_back(
            [&docs]()
            {
                for (int iter = 0; iter < 100; iter++)
                {
                    EXPECT_EQ(docs.size(), 50u);
                    const uint32_t id = static_cast<uint32_t>(iter % 50);
                    EXPECT_EQ(docs.getDocId(id), "doc_" + std::to_string(id));
                    EXPECT_EQ(docs.getTokenCount(id), static_cast<uint32_t>(10 + id));
                    EXPECT_GT(docs.getAverageTokenCount(), 0.0);
                }
            });
    }
    for (auto &reader : readers)
    {
        reader.join();
    }
}
