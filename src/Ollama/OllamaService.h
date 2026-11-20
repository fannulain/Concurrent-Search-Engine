#ifndef OLLAMASERVICE_H
#define OLLAMASERVICE_H

#include "Index/Tokenizer.h"
#include "ThreadPool/ThreadPool.h"
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <vector>

struct ExpansionResult
{
    std::string originalQuery;
    std::vector<std::string> expandedTerms;
    std::string error;
};

class OllamaService
{
private:
    std::string host;
    std::string port;
    std::string model;
    const Tokenizer& tokenizer;
    int timeoutMs;
    ThreadPool workerPool;

    ExpansionResult performExpansion(const std::string& query) const;

public:
    OllamaService(std::string host, std::string port, std::string model, const Tokenizer& tokenizer,
                  int timeoutMs = 30000);
    ~OllamaService();

    OllamaService(const OllamaService&) = delete;
    OllamaService& operator=(const OllamaService&) = delete;

    bool checkHealth(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const;
    std::future<ExpansionResult> expandQuery(const std::string& query);
    void expandQueryAsync(const std::string& query, std::function<void(ExpansionResult)> onComplete);

    const std::string& getModel() const { return model; }
};

#endif // OLLAMASERVICE_H
