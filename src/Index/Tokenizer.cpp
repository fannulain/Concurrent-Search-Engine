#include "Tokenizer.h"

Tokenizer::Tokenizer()
    : stopWords({
          "about",      "above",      "after",   "again",     "against",   "ain",       "all",       "already",
          "also",       "although",   "always",  "am",        "among",     "an",        "and",       "any",
          "are",        "aren",       "around",  "as",        "at",        "be",        "because",   "been",
          "before",     "being",      "below",   "between",   "both",      "but",       "by",        "can",
          "cannot",     "could",      "couldn",  "did",       "didn",      "do",        "does",      "doesn",
          "doing",      "don",        "down",    "during",    "each",      "either",    "even",      "every",
          "few",        "for",        "from",    "further",   "had",       "hadn",      "has",       "hasn",
          "have",       "haven",      "having",  "he",        "hence",     "her",       "here",      "hers",
          "herself",    "him",        "himself", "his",       "how",       "however",   "if",        "in",
          "into",       "is",         "isn",     "it",        "its",       "itself",    "just",      "ll",
          "ma",         "many",       "me",      "might",     "mightn",    "more",      "most",      "much",
          "must",       "mustn",      "my",      "myself",    "needn",     "neither",   "never",     "no",
          "nor",        "not",        "now",     "of",        "off",       "often",     "on",        "once",
          "only",       "or",         "other",   "our",       "ours",      "ourselves", "out",       "over",
          "own",        "re",         "same",    "shall",     "shan",      "she",       "should",    "shouldn",
          "since",      "so",         "some",    "still",     "such",      "than",      "that",      "the",
          "their",      "theirs",     "them",    "themselves", "then",     "there",     "therefore", "these",
          "they",       "this",       "those",   "though",    "through",   "thus",      "to",        "too",
          "under",      "until",      "up",      "upon",      "ve",        "very",      "was",       "wasn",
          "we",         "were",       "weren",   "what",      "when",      "where",     "whether",   "which",
          "while",      "who",        "whom",    "whose",     "why",       "will",      "with",      "within",
          "without",    "won",        "would",   "wouldn",    "yet",       "you",       "your",      "yours",
          "yourself",   "yourselves",
      })
{
}

template <typename Callback>
void Tokenizer::forEachToken(std::string_view text, Callback&& onToken) const
{
    std::string currentToken;
    currentToken.reserve(32);

    auto flushToken = [&]()
    {
        if (currentToken.size() >= 2 && !isStopWord(currentToken))
        {
            onToken(currentToken);
        }
        currentToken.clear();
    };

    for (unsigned char ch : text)
    {
        if (ch >= 'a' && ch <= 'z')
        {
            currentToken.push_back(static_cast<char>(ch));
        }
        else if (ch >= 'A' && ch <= 'Z')
        {
            currentToken.push_back(static_cast<char>(ch - 'A' + 'a'));
        }
        else if (!currentToken.empty())
        {
            flushToken();
        }
    }

    if (!currentToken.empty())
    {
        flushToken();
    }
}

bool Tokenizer::isStopWord(std::string_view token) const
{
    return stopWords.find(token) != stopWords.end();
}

std::vector<std::string> Tokenizer::tokenize(std::string_view text) const
{
    std::vector<std::string> tokens;
    forEachToken(text, [&tokens](const std::string& token) { tokens.push_back(token); });
    return tokens;
}

uint32_t Tokenizer::tokenizeToTermFreq(std::string_view text,
                                       std::unordered_map<std::string, uint32_t>& termFreq) const
{
    uint32_t tokenCount = 0;
    forEachToken(text,
                 [&termFreq, &tokenCount](const std::string& token)
                 {
                     auto [it, inserted] = termFreq.try_emplace(token, 1u);
                     if (!inserted)
                     {
                         it->second++;
                     }
                     tokenCount++;
                 });
    return tokenCount;
}
