#include "Index/Tokenizer.h"
#include "Containers/HashMap.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

TEST(TokenizerTest, NormalizesCaseAndSplitsOnNonLetters)
{
    const Tokenizer tokenizer;
    const std::vector<std::string> tokens = tokenizer.tokenize("Hello, WORLD! 123 test-case_42.");
    const std::vector<std::string> expected = {"hello", "world", "test", "case"};
    EXPECT_EQ(tokens, expected);
}

TEST(TokenizerTest, RemovesShortTokensAndStopWords)
{
    const Tokenizer tokenizer;
    const std::vector<std::string> tokens = tokenizer.tokenize("I a to the and is x y search engine");
    const std::vector<std::string> expected = {"search", "engine"};
    EXPECT_EQ(tokens, expected);
}

TEST(TokenizerTest, TermFreqMatchesTokenizeCounts)
{
    const Tokenizer tokenizer;
    const std::string text = "Space travel, space ships, and distant stars! Stars shine in space.";

    const std::vector<std::string> tokens = tokenizer.tokenize(text);
    HashMap<std::string, uint32_t> termFreq;
    const uint32_t count = tokenizer.tokenizeToTermFreq(text, termFreq);

    EXPECT_EQ(count, tokens.size());
    EXPECT_EQ(termFreq["space"], 3u);
    EXPECT_EQ(termFreq["stars"], 2u);
    EXPECT_EQ(termFreq["travel"], 1u);
    EXPECT_EQ(termFreq["ships"], 1u);
    EXPECT_EQ(termFreq["distant"], 1u);
    EXPECT_EQ(termFreq["shine"], 1u);
    EXPECT_EQ(termFreq.find("and"), termFreq.end());
    EXPECT_EQ(termFreq.find("in"), termFreq.end());
}
