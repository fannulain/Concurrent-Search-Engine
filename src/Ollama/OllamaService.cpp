#include "OllamaService.h"
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <exception>
#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_set>
#include <utility>

namespace
{

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using nlohmann::json;

constexpr char USER_AGENT[] = "concurrent-search-engine/0.1";
constexpr size_t MAX_EXPANDED_TERMS = 10;

bool modelListContains(const std::string &body, const std::string &targetModel)
{
    const auto parsed = json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("models") || !parsed["models"].is_array())
    {
        return false;
    }

    for (const auto &entry : parsed["models"])
    {
        if (!entry.is_object()) continue;
        const std::string name = entry.value("name", "");
        if (name == targetModel || name == targetModel + ":latest")
        {
            return true;
        }
    }
    return false;
}

net::awaitable<bool> fetchHasModel(std::string host, std::string port, std::string model,
                                   std::chrono::milliseconds timeout)
{
    auto executor = co_await net::this_coro::executor;
    tcp::resolver resolver(executor);
    beast::tcp_stream stream(executor);

    const auto endpoints = co_await resolver.async_resolve(host, port, net::use_awaitable);
    stream.expires_after(timeout);
    co_await stream.async_connect(endpoints, net::use_awaitable);

    http::request<http::empty_body> request{http::verb::get, "/api/tags", 11};
    request.set(http::field::host, host);
    request.set(http::field::user_agent, USER_AGENT);
    co_await http::async_write(stream, request, net::use_awaitable);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, buffer, response, net::use_awaitable);

    beast::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);

    co_return response.result() == http::status::ok &&modelListContains(response.body(), model);
}

struct GenerateHttpResponse
{
    unsigned int statusCode = 0;
    std::string body;
};

net::awaitable<GenerateHttpResponse> fetchGenerate(std::string host, std::string port, std::string requestBody,
                                                   std::chrono::milliseconds timeout)
{
    auto executor = co_await net::this_coro::executor;
    tcp::resolver resolver(executor);
    beast::tcp_stream stream(executor);

    const auto endpoints = co_await resolver.async_resolve(host, port, net::use_awaitable);
    stream.expires_after(timeout);
    co_await stream.async_connect(endpoints, net::use_awaitable);

    http::request<http::string_body> request{http::verb::post, "/api/generate", 11};
    request.set(http::field::host, host);
    request.set(http::field::user_agent, USER_AGENT);
    request.set(http::field::content_type, "application/json");
    request.body() = std::move(requestBody);
    request.prepare_payload();

    co_await http::async_write(stream, request, net::use_awaitable);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, buffer, response, net::use_awaitable);

    beast::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);

    co_return GenerateHttpResponse{response.result_int(), std::move(response.body())};
}

bool extractRawTerms(const std::string &responseStr, std::vector<std::string> &outRawTerms)
{
    const json inner = json::parse(responseStr, nullptr, false);
    if (inner.is_discarded()) return false;

    const json *termsArray = nullptr;
    if (inner.is_object() && inner.contains("terms") && inner["terms"].is_array())
    {
        termsArray = &inner["terms"];
    }
    else if (inner.is_array())
    {
        termsArray = &inner;
    }
    else if (inner.is_object())
    {
        for (const auto &[key, val] : inner.items())
        {
            (void)key;
            if (val.is_array())
            {
                termsArray = &val;
                break;
            }
        }
    }

    if (termsArray == nullptr) return false;

    for (const auto &item : *termsArray)
    {
        if (item.is_string())
        {
            outRawTerms.push_back(item.get<std::string>());
        }
    }
    return true;
}

} // namespace

OllamaService::OllamaService(std::string host, std::string port, std::string model, const Tokenizer &tokenizer,
                             int timeoutMs)
    : host(std::move(host)), port(std::move(port)), model(std::move(model)), tokenizer(tokenizer), timeoutMs(timeoutMs),
      workerPool(1)
{
}

OllamaService::~OllamaService()
{
    workerPool.stop(true);
}

bool OllamaService::checkHealth(std::chrono::milliseconds timeout) const
{
    net::io_context ioc;
    std::optional<bool> result;
    net::co_spawn(ioc, fetchHasModel(host, port, model, timeout),
                  [&result](std::exception_ptr error, bool hasModel) { result = !error && hasModel; });
    ioc.run_for(timeout);
    return result.value_or(false);
}

ExpansionResult OllamaService::performExpansion(const std::string &query) const
{
    ExpansionResult result;
    result.originalQuery = query;

    try
    {
        const std::string prompt =
            "Given the search query \"" + query +
            "\", return a JSON object {\"terms\": [...]} with up to 10 English keywords and synonyms for "
            "full-text search. Output valid JSON only.";

        const json payload = {
            {"model", model},
            {"prompt", prompt},
            {"stream", false},
            {"format", "json"},
        };

        const std::chrono::milliseconds timeout(timeoutMs);
        net::io_context ioc;
        std::optional<GenerateHttpResponse> httpResponse;
        std::exception_ptr requestError;

        net::co_spawn(ioc, fetchGenerate(host, port, payload.dump(), timeout),
                      [&httpResponse, &requestError](std::exception_ptr error, GenerateHttpResponse resp)
                      {
                          if (error)
                          {
                              requestError = error;
                          }
                          else
                          {
                              httpResponse = std::move(resp);
                          }
                      });

        ioc.run_for(timeout);

        if (requestError)
        {
            std::rethrow_exception(requestError);
        }
        if (!httpResponse.has_value())
        {
            result.error = "Ollama request timed out";
            return result;
        }
        if (httpResponse->statusCode != 200)
        {
            result.error = "Ollama returned HTTP " + std::to_string(httpResponse->statusCode);
            return result;
        }

        const json outer = json::parse(httpResponse->body, nullptr, false);
        if (outer.is_discarded() || !outer.is_object() || !outer.contains("response") || !outer["response"].is_string())
        {
            result.error = "Invalid response payload from Ollama";
            return result;
        }

        std::vector<std::string> rawTerms;
        if (!extractRawTerms(outer["response"].get<std::string>(), rawTerms))
        {
            result.error = "Unparsable JSON terms in Ollama response";
            return result;
        }

        std::unordered_set<std::string> originalTokens;
        for (const std::string &tok : tokenizer.tokenize(query))
        {
            originalTokens.insert(tok);
        }

        std::unordered_set<std::string> seenExpanded;
        for (const std::string &rawTerm : rawTerms)
        {
            for (const std::string &tok : tokenizer.tokenize(rawTerm))
            {
                if (originalTokens.find(tok) != originalTokens.end()) continue;
                if (!seenExpanded.insert(tok).second) continue;

                result.expandedTerms.push_back(tok);
                if (result.expandedTerms.size() >= MAX_EXPANDED_TERMS)
                {
                    return result;
                }
            }
        }
    }
    catch (const std::exception &e)
    {
        result.error = e.what();
    }

    return result;
}

std::future<ExpansionResult> OllamaService::expandQuery(const std::string &query)
{
    return workerPool.addTask([this, query]() { return performExpansion(query); });
}

void OllamaService::expandQueryAsync(const std::string &query, std::function<void(ExpansionResult)> onComplete)
{
    workerPool.addTask(
        [this, query, cb = std::move(onComplete)]() mutable
        {
            ExpansionResult res = performExpansion(query);
            if (cb) cb(std::move(res));
        });
}
