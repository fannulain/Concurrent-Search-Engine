#include "Index/IndexerService.h"
#include "Index/Tokenizer.h"
#include "Search/SearchEngine.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

class TempDirGuard
{
private:
    std::filesystem::path dirPath;

public:
    TempDirGuard()
    {
        static std::atomic<int> counter{ 0 };
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        dirPath = std::filesystem::temp_directory_path() /
                  ("cse_test_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(dirPath);
    }

    ~TempDirGuard()
    {
        std::error_code ec;
        std::filesystem::remove_all(dirPath, ec);
    }

    const std::filesystem::path& path() const { return dirPath; }
};

void writeTextFile(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.is_open());
    out << content;
}

} // namespace

TEST(IndexerAndSearchTest, StripsGutenbergMarkersAndChunksDeterministically)
{
    TempDirGuard tempDir;

    std::string bookContent = "Title: Test Book\n";
    bookContent += "gutenberglicensebait should not be indexed\n";
    bookContent += "*** START OF THIS PROJECT GUTENBERG EBOOK TEST ***\n";
    for (int i = 0; i < 1200; i++)
    {
        bookContent += (i == 50 ? "nebula " : "cosmos ");
    }
    bookContent += "\n*** END OF THIS PROJECT GUTENBERG EBOOK TEST ***\n";
    bookContent += "footerbait should also be ignored\n";

    writeTextFile(tempDir.path() / "100.txt", bookContent);

    const Tokenizer tokenizer;
    const IndexerService indexer(tokenizer, tempDir.path().string(), 16);

    const auto snap1 = indexer.buildIndex(1);
    const auto snap4 = indexer.buildIndex(4);

    EXPECT_EQ(snap1->stats.documents, 2u);
    EXPECT_EQ(snap4->stats.documents, 2u);
    EXPECT_EQ(snap1->index.getTermCount(), snap4->index.getTermCount());

    EXPECT_TRUE(snap1->index.getPostings("gutenberglicensebait").empty());
    EXPECT_TRUE(snap1->index.getPostings("footerbait").empty());
    ASSERT_EQ(snap1->index.getPostings("nebula").size(), 1u);
    ASSERT_EQ(snap1->index.getPostings("cosmos").size(), 2u);

    const SearchEngine engine(tokenizer);
    const auto results1 = engine.searchQuery(*snap1, "nebula", 10);
    const auto results4 = engine.searchQuery(*snap4, "nebula", 10);

    ASSERT_EQ(results1.size(), 1u);
    ASSERT_EQ(results4.size(), 1u);
    EXPECT_EQ(results1[0].docId, "100_0");
    EXPECT_EQ(results4[0].docId, "100_0");
    EXPECT_DOUBLE_EQ(results1[0].score, 1.0);
    EXPECT_DOUBLE_EQ(results4[0].score, 1.0);
    EXPECT_NE(results1[0].snippet.find("nebula"), std::string::npos);
}

TEST(IndexerAndSearchTest, ComputesTfIdfNormalizationTieBreakingAndWeights)
{
    TempDirGuard tempDir;

    writeTextFile(tempDir.path() / "bookA.txt", "galaxy stars rocket galaxy galaxy");
    writeTextFile(tempDir.path() / "bookB.txt", "galaxy planet earth mars");
    writeTextFile(tempDir.path() / "bookC.txt", "astronomy telescope orbit");

    const Tokenizer tokenizer;
    const IndexerService indexer(tokenizer, tempDir.path().string(), 16);
    const auto snap = indexer.buildIndex(2);

    const SearchEngine engine(tokenizer);

    const auto keywordResults = engine.searchQuery(*snap, "galaxy", 10);
    ASSERT_EQ(keywordResults.size(), 2u);
    EXPECT_EQ(keywordResults[0].docId, "bookA_0");
    EXPECT_DOUBLE_EQ(keywordResults[0].score, 1.0);
    EXPECT_EQ(keywordResults[1].docId, "bookB_0");
    EXPECT_LT(keywordResults[1].score, 1.0);

    // expanded term gets weight 0.5 vs original term weight 1.0
    const std::vector<WeightedTerm> weighted = engine.buildWeightedTerms("rocket", { "orbit" });
    const auto weightedResults = engine.search(*snap, weighted, 10);
    ASSERT_EQ(weightedResults.size(), 2u);
    EXPECT_EQ(weightedResults[0].docId, "bookA_0");
    EXPECT_EQ(weightedResults[1].docId, "bookC_0");
    EXPECT_DOUBLE_EQ(weightedResults[0].score, 1.0);
}

TEST(IndexerAndSearchTest, DeterministicTieBreakingByDocId)
{
    TempDirGuard tempDir;

    writeTextFile(tempDir.path() / "docB.txt", "quantum mechanics wave");
    writeTextFile(tempDir.path() / "docA.txt", "quantum mechanics wave");

    const Tokenizer tokenizer;
    const IndexerService indexer(tokenizer, tempDir.path().string(), 8);
    const auto snap = indexer.buildIndex(2);

    const SearchEngine engine(tokenizer);
    const auto results = engine.searchQuery(*snap, "quantum", 10);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].docId, "docA_0");
    EXPECT_EQ(results[1].docId, "docB_0");
    EXPECT_FALSE(std::isnan(results[0].score));
}

TEST(IndexerAndSearchTest, HandlesSingleDocumentWithoutNaN)
{
    TempDirGuard tempDir;
    writeTextFile(tempDir.path() / "only.txt", "solitary lighthouse ocean");

    const Tokenizer tokenizer;
    const IndexerService indexer(tokenizer, tempDir.path().string(), 8);
    const auto snap = indexer.buildIndex(1);

    const SearchEngine engine(tokenizer);
    const auto results = engine.searchQuery(*snap, "lighthouse", 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].docId, "only_0");
    EXPECT_DOUBLE_EQ(results[0].score, 1.0);
}

TEST(IndexerAndSearchTest, ThrowsOnMissingOrEmptyDataset)
{
    const Tokenizer tokenizer;
    const IndexerService missingIndexer(tokenizer, "/nonexistent_cse_dataset_dir_12345", 8);
    EXPECT_THROW(missingIndexer.buildIndex(1), std::runtime_error);

    TempDirGuard emptyDir;
    const IndexerService emptyIndexer(tokenizer, emptyDir.path().string(), 8);
    EXPECT_THROW(emptyIndexer.buildIndex(1), std::runtime_error);
}
