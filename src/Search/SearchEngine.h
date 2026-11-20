#ifndef SEARCHENGINE_H
#define SEARCHENGINE_H

#include "Index/IndexSnapshot.h"
#include "Index/Tokenizer.h"
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

struct WeightedTerm
{
    std::string term;
    double weight = 1.0;
};

struct SearchResult
{
    std::string docId;
    double score = 0.0;
    std::string snippet;
};

class SearchEngine
{
private:
    const Tokenizer& tokenizer;

    static std::string buildSnippet(std::string_view text, const std::vector<WeightedTerm>& terms);

public:
    explicit SearchEngine(const Tokenizer& tokenizer);

    std::vector<WeightedTerm> buildWeightedTerms(std::string_view originalQuery,
                                                   const std::vector<std::string>& expandedTerms) const;

    std::vector<SearchResult> search(const IndexSnapshot& snapshot, const std::vector<WeightedTerm>& terms,
                                     size_t limit) const;

    std::vector<SearchResult> searchQuery(const IndexSnapshot& snapshot, std::string_view query, size_t limit) const;
};

#endif // SEARCHENGINE_H
