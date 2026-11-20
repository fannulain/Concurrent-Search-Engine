#include "ClientSession.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

namespace
{

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using nlohmann::json;

constexpr std::chrono::seconds IDLE_TIMEOUT{30};
constexpr int DEFAULT_WS_LIMIT = 10;
constexpr int MIN_WS_LIMIT = 1;
constexpr int MAX_WS_LIMIT = 100;

bool isBenignError(const beast::error_code &ec)
{
    return ec == beast::error::timeout || ec == net::error::operation_aborted || ec == net::error::connection_reset ||
           ec == http::error::end_of_stream || ec == websocket::error::closed || ec == net::error::eof;
}

std::string_view stripQueryString(beast::string_view target)
{
    const std::string_view sv(target.data(), target.size());
    return sv.substr(0, sv.find('?'));
}

std::string trimQuery(std::string_view input)
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

bool parseLimitInt(const json &value, int &outVal)
{
    if (!value.is_number_integer()) return false;
    if (value.is_number_unsigned())
    {
        const uint64_t u = value.get<uint64_t>();
        if (u < MIN_WS_LIMIT || u > MAX_WS_LIMIT) return false;
        outVal = static_cast<int>(u);
        return true;
    }
    const long long s = value.get<long long>();
    if (s < MIN_WS_LIMIT || s > MAX_WS_LIMIT) return false;
    outVal = static_cast<int>(s);
    return true;
}

} // namespace

WebSocketSession::WebSocketSession(tcp::socket &&socket, RequestHandler &requestHandler)
    : ws(std::move(socket)), requestHandler(requestHandler)
{
}

void WebSocketSession::start(HttpRequest request)
{
    beast::get_lowest_layer(ws).expires_never();
    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws.set_option(websocket::stream_base::decorator([](websocket::response_type &res)
                                                    { res.set(http::field::server, "concurrent-search-engine"); }));
    ws.text(true);

    ws.async_accept(request, beast::bind_front_handler(&WebSocketSession::onAccept, shared_from_this()));
}

void WebSocketSession::onAccept(beast::error_code ec)
{
    if (ec)
    {
        if (!isBenignError(ec))
        {
            std::cerr << "WebSocket accept error: " << ec.message() << std::endl;
        }
        return;
    }
    doRead();
}

void WebSocketSession::doRead()
{
    buffer.consume(buffer.size());
    ws.async_read(buffer, beast::bind_front_handler(&WebSocketSession::onRead, shared_from_this()));
}

void WebSocketSession::onRead(beast::error_code ec, std::size_t /*bytesTransferred*/)
{
    if (ec)
    {
        if (!isBenignError(ec))
        {
            std::cerr << "WebSocket read error: " << ec.message() << std::endl;
        }
        return;
    }

    const std::string payload = beast::buffers_to_string(buffer.data());
    buffer.consume(buffer.size());

    const json msg = json::parse(payload, nullptr, false);
    if (msg.is_discarded() || !msg.is_object())
    {
        return sendError("Message must be a valid JSON object");
    }

    if (!msg.contains("query") || !msg["query"].is_string())
    {
        return sendError("Field 'query' must be a non-empty string");
    }

    const std::string query = trimQuery(msg["query"].get<std::string>());
    if (query.empty())
    {
        return sendError("Field 'query' must not be empty");
    }

    int limit = DEFAULT_WS_LIMIT;
    if (msg.contains("limit") && !parseLimitInt(msg["limit"], limit))
    {
        return sendError("Field 'limit' must be an integer in [1, 100]");
    }

    // release network thread while sequential Ollama worker executes expansion
    auto executor = ws.get_executor();
    try
    {
        requestHandler.getOllamaService().expandQueryAsync(
            query,
            [self = shared_from_this(), executor, limit](ExpansionResult expansion) mutable
            {
                net::post(executor, [self = std::move(self), exp = std::move(expansion), limit]() mutable
                          { self->onExpansionComplete(std::move(exp), limit); });
            });
    }
    catch (const std::exception &e)
    {
        sendError(e.what());
    }
}

void WebSocketSession::sendError(const std::string &message)
{
    outgoingMessage = json{{"stage", "error"}, {"data", {{"message", message}}}}.dump();
    ws.async_write(net::buffer(outgoingMessage),
                   beast::bind_front_handler(&WebSocketSession::onPipelineWritten, shared_from_this()));
}

void WebSocketSession::onExpansionComplete(ExpansionResult expansion, int limit)
{
    json stage1Data = {
        {"original", expansion.originalQuery},
        {"expanded_terms", expansion.expandedTerms},
    };
    if (!expansion.error.empty())
    {
        stage1Data["error"] = expansion.error;
    }

    outgoingMessage = json{{"stage", "query_expansion"}, {"data", std::move(stage1Data)}}.dump();
    ws.async_write(
        net::buffer(outgoingMessage),
        beast::bind_front_handler(&WebSocketSession::onStage1Written, shared_from_this(), std::move(expansion), limit));
}

void WebSocketSession::onStage1Written(ExpansionResult expansion, int limit, beast::error_code ec,
                                       std::size_t /*bytesTransferred*/)
{
    if (ec)
    {
        if (!isBenignError(ec))
        {
            std::cerr << "WebSocket write error: " << ec.message() << std::endl;
        }
        return;
    }

    const std::shared_ptr<const IndexSnapshot> snap = requestHandler.getSnapshot();
    json resultsJson = json::array();
    uint64_t timeMs = 0;

    if (snap)
    {
        const auto startTime = std::chrono::steady_clock::now();
        const SearchEngine &engine = requestHandler.getSearchEngine();
        const std::vector<WeightedTerm> terms =
            engine.buildWeightedTerms(expansion.originalQuery, expansion.expandedTerms);
        const std::vector<SearchResult> results = engine.search(*snap, terms, static_cast<size_t>(limit));
        const auto endTime = std::chrono::steady_clock::now();

        timeMs = static_cast<uint64_t>(
            std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count()));

        for (const SearchResult &res : results)
        {
            resultsJson.push_back(json{{"doc_id", res.docId}, {"score", res.score}, {"snippet", res.snippet}});
        }
    }

    outgoingMessage =
        json{{"stage", "keyword_search"}, {"data", {{"results", std::move(resultsJson)}, {"time_ms", timeMs}}}}.dump();

    ws.async_write(net::buffer(outgoingMessage),
                   beast::bind_front_handler(&WebSocketSession::onStage2Written, shared_from_this()));
}

void WebSocketSession::onStage2Written(beast::error_code ec, std::size_t /*bytesTransferred*/)
{
    if (ec)
    {
        if (!isBenignError(ec))
        {
            std::cerr << "WebSocket write error: " << ec.message() << std::endl;
        }
        return;
    }

    outgoingMessage = json{{"stage", "done"}}.dump();
    ws.async_write(net::buffer(outgoingMessage),
                   beast::bind_front_handler(&WebSocketSession::onPipelineWritten, shared_from_this()));
}

void WebSocketSession::onPipelineWritten(beast::error_code ec, std::size_t /*bytesTransferred*/)
{
    if (ec)
    {
        if (!isBenignError(ec))
        {
            std::cerr << "WebSocket write error: " << ec.message() << std::endl;
        }
        return;
    }

    doRead();
}

ClientSession::ClientSession(tcp::socket &&socket, RequestHandler &requestHandler)
    : stream(std::move(socket)), requestHandler(requestHandler)
{
}

void ClientSession::start()
{
    net::dispatch(stream.get_executor(), beast::bind_front_handler(&ClientSession::doRead, shared_from_this()));
}

void ClientSession::doRead()
{
    request = {};
    stream.expires_after(IDLE_TIMEOUT);
    http::async_read(stream, buffer, request, beast::bind_front_handler(&ClientSession::onRead, shared_from_this()));
}

void ClientSession::onRead(beast::error_code ec, std::size_t /*bytesTransferred*/)
{
    if (ec == http::error::end_of_stream)
    {
        return doClose();
    }
    if (ec)
    {
        if (!isBenignError(ec))
        {
            std::cerr << "Read error: " << ec.message() << std::endl;
        }
        return;
    }

    if (websocket::is_upgrade(request))
    {
        if (stripQueryString(request.target()) != "/ws/search")
        {
            response = HttpResponse{http::status::not_found, request.version()};
            response.set(http::field::server, "concurrent-search-engine");
            response.set(http::field::content_type, "application/json");
            response.keep_alive(false);
            response.body() = json{{"error", "WebSocket endpoint not found"}}.dump();
            response.prepare_payload();
            http::async_write(stream, response,
                              beast::bind_front_handler(&ClientSession::onWrite, shared_from_this(), false));
            return;
        }

        std::make_shared<WebSocketSession>(stream.release_socket(), requestHandler)->start(std::move(request));
        return;
    }

    response = requestHandler.handle(request);
    const bool keepAlive = response.keep_alive();
    http::async_write(stream, response,
                      beast::bind_front_handler(&ClientSession::onWrite, shared_from_this(), keepAlive));
}

void ClientSession::onWrite(bool keepAlive, beast::error_code ec, std::size_t /*bytesTransferred*/)
{
    if (ec)
    {
        if (!isBenignError(ec))
        {
            std::cerr << "Write error: " << ec.message() << std::endl;
        }
        return;
    }
    if (!keepAlive)
    {
        return doClose();
    }
    doRead();
}

void ClientSession::doClose()
{
    beast::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
}
