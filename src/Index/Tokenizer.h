#ifndef TOKENIZER_H
#define TOKENIZER_H

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Tokenizer
{
private:
    std::unordered_set<std::string_view> stopWords;

    template <typename Callback>
    void forEachToken(std::string_view text, Callback&& onToken) const;

public:
    Tokenizer();

    bool isStopWord(std::string_view token) const;
    std::vector<std::string> tokenize(std::string_view text) const;
    uint32_t tokenizeToTermFreq(std::string_view text, std::unordered_map<std::string, uint32_t>& termFreq) const;
};

#endif // TOKENIZER_H
