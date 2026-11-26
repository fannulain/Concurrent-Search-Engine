#include "SearchEngine.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{

constexpr size_t SNIPPET_TARGET_LENGTH = 200;
constexpr size_t SNIPPET_LOOKBACK = 60;
constexpr double BM25_K1 = 1.2;
constexpr double BM25_B = 0.75;

struct ScoredCandidate
{
    uint32_t internalId = 0;
    double score = 0.0;
};

struct ScoreBufferResetGuard
{
    std::vector<double> &scores;
    std::vector<uint32_t> &touched;
    ~ScoreBufferResetGuard()
    {
        for (const uint32_t id : touched)
        {
            scores[id] = 0.0;
        }
        touched.clear();
    }
};

size_t findFirstTermMatchOffset(std::string_view text, const std::unordered_set<std::string_view> &queryTerms)
{
    std::string token;
    token.reserve(32);
    size_t tokenStart = 0;

    for (size_t i = 0; i < text.size(); i++)
    {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch >= 'a' && ch <= 'z')
        {
            if (token.empty()) tokenStart = i;
            token.push_back(static_cast<char>(ch));
        }
        else if (ch >= 'A' && ch <= 'Z')
        {
            if (token.empty()) tokenStart = i;
            token.push_back(static_cast<char>(ch - 'A' + 'a'));
        }
        else if (!token.empty())
        {
            if (queryTerms.find(token) != queryTerms.end()) return tokenStart;
            token.clear();
        }
    }

    if (!token.empty() && queryTerms.find(token) != queryTerms.end())
    {
        return tokenStart;
    }

    return 0;
}

std::string normalizeSnippetWhitespace(std::string_view slice)
{
    std::string cleaned;
    cleaned.reserve(slice.size());
    bool previousSpace = true;

    for (unsigned char ch : slice)
    {
        if (std::isspace(ch))
        {
            if (!previousSpace)
            {
                cleaned.push_back(' ');
                previousSpace = true;
            }
        }
        else
        {
            cleaned.push_back(static_cast<char>(ch));
            previousSpace = false;
        }
    }

    if (!cleaned.empty() && cleaned.back() == ' ')
    {
        cleaned.pop_back();
    }
    return cleaned;
}

} // namespace

SearchEngine::SearchEngine(const Tokenizer &tokenizer) : tokenizer(tokenizer) {}

std::string SearchEngine::buildSnippet(std::string_view text, const std::vector<WeightedTerm> &terms)
{
    if (text.empty()) return {};

    std::unordered_set<std::string_view> queryTerms;
    queryTerms.reserve(terms.size());
    for (const auto &wt : terms)
    {
        queryTerms.insert(wt.term);
    }

    const size_t matchOffset = findFirstTermMatchOffset(text, queryTerms);
    size_t windowStart = matchOffset > SNIPPET_LOOKBACK ? matchOffset - SNIPPET_LOOKBACK : 0;
    size_t windowEnd = std::min(text.size(), windowStart + SNIPPET_TARGET_LENGTH);

    if (windowEnd - windowStart < SNIPPET_TARGET_LENGTH && windowEnd == text.size() &&
        text.size() > SNIPPET_TARGET_LENGTH)
    {
        windowStart = text.size() - SNIPPET_TARGET_LENGTH;
    }

    if (windowStart > 0)
    {
        size_t spacePos = windowStart;
        while (spacePos < matchOffset && !std::isspace(static_cast<unsigned char>(text[spacePos])))
        {
            spacePos++;
        }
        if (spacePos < matchOffset)
        {
            windowStart = spacePos + 1;
        }
    }

    if (windowEnd < text.size())
    {
        size_t spacePos = windowEnd;
        while (spacePos > windowStart && !std::isspace(static_cast<unsigned char>(text[spacePos])))
        {
            spacePos--;
        }
        if (spacePos > windowStart + SNIPPET_TARGET_LENGTH / 2)
        {
            windowEnd = spacePos;
        }
    }

    std::string snippetBody = normalizeSnippetWhitespace(text.substr(windowStart, windowEnd - windowStart));
    if (snippetBody.empty()) return {};

    std::string result;
    if (windowStart > 0) result += "...";
    result += snippetBody;
    if (windowEnd < text.size()) result += "...";
    return result;
}

std::vector<WeightedTerm> SearchEngine::buildWeightedTerms(std::string_view originalQuery,
                                                           const std::vector<std::string> &expandedTerms) const
{
    std::vector<WeightedTerm> result;
    std::unordered_set<std::string> seen;

    for (const std::string &token : tokenizer.tokenize(originalQuery))
    {
        if (seen.insert(token).second)
        {
            result.push_back(WeightedTerm{token, 1.0});
        }
    }

    for (const std::string &rawExpanded : expandedTerms)
    {
        for (const std::string &token : tokenizer.tokenize(rawExpanded))
        {
            if (seen.insert(token).second)
            {
                result.push_back(WeightedTerm{token, 0.5});
            }
        }
    }

    return result;
}

std::vector<SearchResult> SearchEngine::search(const IndexSnapshot &snapshot, const std::vector<WeightedTerm> &terms,
                                               size_t limit) const
{
    if (terms.empty() || limit == 0) return {};

    const uint32_t totalDocs = snapshot.index.getDocumentCount();
    if (totalDocs == 0) return {};

    const double avgDocLen = std::max(1.0, snapshot.docs.getAverageTokenCount());
    const size_t requiredSize = std::max<size_t>(totalDocs, snapshot.docs.size());

    thread_local std::vector<double> scoreBuffer;
    thread_local std::vector<uint32_t> touchedIds;
    if (scoreBuffer.size() < requiredSize)
    {
        scoreBuffer.resize(requiredSize, 0.0);
    }
    ScoreBufferResetGuard resetGuard{scoreBuffer, touchedIds};

    for (const WeightedTerm &wt : terms)
    {
        if (wt.weight <= 0.0 || wt.term.empty()) continue;

        snapshot.index.visitPostings(
            wt.term,
            [&](const DynamicArray<Posting> &postings)
            {
                if (postings.empty()) return;

                const double df = static_cast<double>(postings.size());
                const double idf =
                    std::log(1.0 + std::max(0.0, static_cast<double>(totalDocs) - df + 0.5) / (df + 0.5));
                if (idf <= 0.0) return;

                for (const Posting &posting : postings)
                {
                    if (posting.docId >= scoreBuffer.size()) continue;
                    const uint32_t docTokenCount = snapshot.docs.getTokenCount(posting.docId);
                    if (docTokenCount == 0) continue;

                    const double tf = static_cast<double>(posting.tf);
                    const double docLen = static_cast<double>(docTokenCount);
                    const double denom = tf + BM25_K1 * (1.0 - BM25_B + BM25_B * (docLen / avgDocLen));
                    const double termScore = wt.weight * idf * (tf * (BM25_K1 + 1.0)) / denom;

                    if (termScore > 0.0)
                    {
                        if (scoreBuffer[posting.docId] == 0.0)
                        {
                            touchedIds.push_back(posting.docId);
                        }
                        scoreBuffer[posting.docId] += termScore;
                    }
                }
            });
    }

    if (touchedIds.empty()) return {};

    double maxRawScore = 0.0;
    for (const uint32_t internalId : touchedIds)
    {
        if (scoreBuffer[internalId] > maxRawScore) maxRawScore = scoreBuffer[internalId];
    }

    std::vector<ScoredCandidate> candidates;
    candidates.reserve(touchedIds.size());

    for (const uint32_t internalId : touchedIds)
    {
        const double normalizedScore = maxRawScore > 0.0 ? (scoreBuffer[internalId] / maxRawScore) : 1.0;
        candidates.push_back(ScoredCandidate{internalId, normalizedScore});
    }

    const size_t resultCount = std::min(limit, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(resultCount),
                      candidates.end(),
                      [&snapshot](const ScoredCandidate &a, const ScoredCandidate &b)
                      {
                          if (a.score != b.score) return a.score > b.score;
                          return snapshot.docs.getDocId(a.internalId) < snapshot.docs.getDocId(b.internalId);
                      });

    std::vector<SearchResult> results;
    results.reserve(resultCount);

    for (size_t i = 0; i < resultCount; i++)
    {
        const ScoredCandidate &candidate = candidates[i];
        const std::string docId = snapshot.docs.getDocId(candidate.internalId);
        const std::string text = snapshot.docs.readText(candidate.internalId);
        results.push_back(SearchResult{docId, candidate.score, buildSnippet(text, terms)});
    }

    return results;
}

std::vector<SearchResult> SearchEngine::searchQuery(const IndexSnapshot &snapshot, std::string_view query,
                                                    size_t limit) const
{
    const std::vector<WeightedTerm> terms = buildWeightedTerms(query, {});
    return search(snapshot, terms, limit);
}
