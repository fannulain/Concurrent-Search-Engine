#include "Index/ConcurrentInvertedIndex.h"
#include <atomic>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

TEST(ConcurrentInvertedIndexTest, AddsAndRetrievesPostings)
{
    ConcurrentInvertedIndex index(16);

    index.addDocument(0, { { "alpha", 2 }, { "beta", 1 } });
    index.addDocument(1, { { "alpha", 5 }, { "gamma", 3 } });

    EXPECT_EQ(index.getDocumentCount(), 2u);
    EXPECT_EQ(index.getTermCount(), 3u);
    EXPECT_EQ(index.getBucketCount(), 16u);

    const std::vector<Posting> alphaPostings = index.getPostings("alpha");
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

    std::atomic<bool> writersDone{ false };
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
                    std::unordered_map<std::string, uint32_t> tf;
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

    for (auto& writer : writers)
    {
        writer.join();
    }
    writersDone.store(true, std::memory_order_release);

    for (auto& reader : readers)
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
