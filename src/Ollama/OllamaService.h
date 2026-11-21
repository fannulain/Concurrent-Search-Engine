#ifndef OLLAMASERVICE_H
#define OLLAMASERVICE_H

#include "Index/Tokenizer.h"
#include "ThreadPool/ThreadPool.h"
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
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
    const Tokenizer &tokenizer;
    int timeoutMs;
    ThreadPool workerPool;
    std::atomic<bool> stopping{false};
    mutable std::mutex activeIocMtx;
    mutable boost::asio::io_context *activeIoc = nullptr;

    ExpansionResult performExpansion(const std::string &query) const;

public:
    OllamaService(std::string host, std::string port, std::string model, const Tokenizer &tokenizer,
                  int timeoutMs = 30000);
    ~OllamaService();

    OllamaService(const OllamaService &) = delete;
    OllamaService &operator=(const OllamaService &) = delete;

    void stop(bool finishTasks = false);
    bool checkHealth(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const;
    std::future<ExpansionResult> expandQuery(const std::string &query);
    void expandQueryAsync(const std::string &query, std::function<void(ExpansionResult)> onComplete);

    const std::string &getModel() const { return model; }
};

#endif // OLLAMASERVICE_H
