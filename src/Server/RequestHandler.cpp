#include "RequestHandler.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace
{

namespace http = boost::beast::http;
using nlohmann::json;

constexpr std::chrono::milliseconds OLLAMA_HEALTH_TIMEOUT{ 2000 };
constexpr int MIN_INDEX_THREADS = 1;
constexpr int MAX_INDEX_THREADS = 64;
constexpr int DEFAULT_SEARCH_LIMIT = 10;
constexpr int MIN_SEARCH_LIMIT = 1;
constexpr int MAX_SEARCH_LIMIT = 100;

struct BuildGuard
{
    std::atomic<bool>& flag;
    ~BuildGuard() { flag.store(false, std::memory_order_release); }
};

HttpResponse makeJsonResponse(const HttpRequest& request, http::status status, const json& body)
{
    HttpResponse response{ status, request.version() };
    response.set(http::field::server, "concurrent-search-engine");
    response.set(http::field::content_type, "application/json");
    response.keep_alive(request.keep_alive());
    response.body() = body.dump();
    response.prepare_payload();
    return response;
}

HttpResponse makeError(const HttpRequest& request, http::status status, std::string_view message)
{
    return makeJsonResponse(request, status, json{ { "error", message } });
}

// strips query parameters such as "/api/health?x=1"
std::string_view pathOf(const HttpRequest& request)
{
    const std::string_view target(request.target().data(), request.target().size());
    return target.substr(0, target.find('?'));
}

std::string trimWhitespace(std::string_view input)
{
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])))
    {
        start++;
    }
    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])))
    {
        end--;
    }
    return std::string(input.substr(start, end - start));
}

bool parseBoundedInt(const json& value, long long minVal, long long maxVal, int& outVal)
{
    if (!value.is_number_integer()) return false;
    if (value.is_number_unsigned())
    {
        const uint64_t unsignedVal = value.get<uint64_t>();
        if (unsignedVal < static_cast<uint64_t>(minVal) || unsignedVal > static_cast<uint64_t>(maxVal))
        {
            return false;
        }
        outVal = static_cast<int>(unsignedVal);
        return true;
    }
    const long long signedVal = value.get<long long>();
    if (signedVal < minVal || signedVal > maxVal) return false;
    outVal = static_cast<int>(signedVal);
    return true;
}

} // namespace

RequestHandler::RequestHandler(OllamaService& ollamaService, const IndexerService& indexerService,
                               const SearchEngine& searchEngine)
    : ollamaService(ollamaService), indexerService(indexerService), searchEngine(searchEngine)
{
}

std::shared_ptr<const IndexSnapshot> RequestHandler::getSnapshot() const
{
    return activeSnapshot.load(std::memory_order_acquire);
}

OllamaService& RequestHandler::getOllamaService()
{
    return ollamaService;
}

const SearchEngine& RequestHandler::getSearchEngine() const
{
    return searchEngine;
}

HttpResponse RequestHandler::handle(const HttpRequest& request)
{
    try
    {
        const std::string_view path = pathOf(request);
        const http::verb method = request.method();

        if (path == "/api/health")
        {
            if (method != http::verb::get)
            {
                return makeError(request, http::status::method_not_allowed, "Method not allowed");
            }
            return handleHealth(request);
        }

        if (path == "/api/index")
        {
            if (method != http::verb::post)
            {
                return makeError(request, http::status::method_not_allowed, "Method not allowed");
            }
            return handleIndex(request);
        }

        if (path == "/api/search")
        {
            if (method != http::verb::post)
            {
                return makeError(request, http::status::method_not_allowed, "Method not allowed");
            }
            return handleSearch(request);
        }

        if (path == "/api/stats")
        {
            if (method != http::verb::get)
            {
                return makeError(request, http::status::method_not_allowed, "Method not allowed");
            }
            return handleStats(request);
        }

        return makeError(request, http::status::not_found, "Not found");
    }
    catch (const std::exception& e)
    {
        std::cerr << "Request handling failed: " << e.what() << std::endl;
        return makeError(request, http::status::internal_server_error, "Internal server error");
    }
}

HttpResponse RequestHandler::handleHealth(const HttpRequest& request) const
{
    const bool ollamaOk = ollamaService.checkHealth(OLLAMA_HEALTH_TIMEOUT);
    return makeJsonResponse(request, http::status::ok, json{ { "status", "ok" }, { "ollama", ollamaOk } });
}

HttpResponse RequestHandler::handleIndex(const HttpRequest& request)
{
    const json body = json::parse(request.body(), nullptr, false);
    if (body.is_discarded() || !body.is_object() || !body.contains("threads"))
    {
        return makeError(request, http::status::bad_request, "Request body must be JSON with integer field 'threads'");
    }

    int threads = 0;
    if (!parseBoundedInt(body["threads"], MIN_INDEX_THREADS, MAX_INDEX_THREADS, threads))
    {
        return makeError(request, http::status::bad_request, "Field 'threads' must be an integer in [1, 64]");
    }

    bool expected = false;
    if (!buildInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return makeError(request, http::status::conflict, "Indexing is already in progress");
    }
    BuildGuard guard{ buildInProgress };

    try
    {
        std::shared_ptr<IndexSnapshot> newSnapshot = indexerService.buildIndex(threads);
        const uint32_t docsIndexed = newSnapshot->stats.documents;
        const uint64_t timeMs = newSnapshot->stats.buildTimeMs;
        activeSnapshot.store(std::const_pointer_cast<const IndexSnapshot>(newSnapshot), std::memory_order_release);

        return makeJsonResponse(request, http::status::ok,
                                json{ { "status", "ok" }, { "documents_indexed", docsIndexed }, { "time_ms", timeMs } });
    }
    catch (const std::exception& e)
    {
        return makeError(request, http::status::internal_server_error, e.what());
    }
}

HttpResponse RequestHandler::handleSearch(const HttpRequest& request) const
{
    const json body = json::parse(request.body(), nullptr, false);
    if (body.is_discarded() || !body.is_object())
    {
        return makeError(request, http::status::bad_request, "Request body must be a JSON object");
    }

    if (!body.contains("query") || !body["query"].is_string())
    {
        return makeError(request, http::status::bad_request, "Field 'query' must be a non-empty string");
    }

    const std::string query = trimWhitespace(body["query"].get<std::string>());
    if (query.empty())
    {
        return makeError(request, http::status::bad_request, "Field 'query' must not be empty");
    }

    if (!body.contains("mode") || !body["mode"].is_string() || body["mode"].get<std::string>() != "keyword")
    {
        return makeError(request, http::status::bad_request, "Field 'mode' must be 'keyword'");
    }

    int limit = DEFAULT_SEARCH_LIMIT;
    if (body.contains("limit"))
    {
        if (!parseBoundedInt(body["limit"], MIN_SEARCH_LIMIT, MAX_SEARCH_LIMIT, limit))
        {
            return makeError(request, http::status::bad_request, "Field 'limit' must be an integer in [1, 100]");
        }
    }

    const std::shared_ptr<const IndexSnapshot> snap = getSnapshot();
    if (!snap)
    {
        return makeJsonResponse(request, http::status::ok, json{ { "results", json::array() }, { "time_ms", 0 } });
    }

    const auto startTime = std::chrono::steady_clock::now();
    const std::vector<SearchResult> results = searchEngine.searchQuery(*snap, query, static_cast<size_t>(limit));
    const auto endTime = std::chrono::steady_clock::now();
    const uint64_t timeMs = static_cast<uint64_t>(
        std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count()));

    json resultsJson = json::array();
    for (const SearchResult& res : results)
    {
        resultsJson.push_back(json{ { "doc_id", res.docId }, { "score", res.score }, { "snippet", res.snippet } });
    }

    return makeJsonResponse(request, http::status::ok, json{ { "results", resultsJson }, { "time_ms", timeMs } });
}

HttpResponse RequestHandler::handleStats(const HttpRequest& request) const
{
    const std::shared_ptr<const IndexSnapshot> snap = getSnapshot();
    if (!snap)
    {
        return makeJsonResponse(request, http::status::ok,
                                json{ { "documents", 0 }, { "index_build_time_ms", 0 }, { "threads_used", 0 } });
    }

    return makeJsonResponse(request, http::status::ok,
                            json{ { "documents", snap->stats.documents },
                                  { "index_build_time_ms", snap->stats.buildTimeMs },
                                  { "threads_used", snap->stats.threadsUsed } });
}
