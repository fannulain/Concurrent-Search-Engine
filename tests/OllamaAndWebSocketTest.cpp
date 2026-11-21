#include "Index/IndexerService.h"
#include "Index/Tokenizer.h"
#include "Ollama/OllamaService.h"
#include "Search/SearchEngine.h"
#include "Server/RequestHandler.h"
#include "Server/SearchServer.h"
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>

namespace
{

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using nlohmann::json;

struct MockHttpResponse
{
    http::status status = http::status::ok;
    std::string body;
    int delayMs = 0;
};

using MockHttpHandler = std::function<MockHttpResponse(const http::request<http::string_body> &)>;

class MockHttpSession : public std::enable_shared_from_this<MockHttpSession>
{
private:
    beast::tcp_stream stream;
    beast::flat_buffer buffer;
    http::request<http::string_body> req;
    http::response<http::string_body> res;
    net::steady_timer timer;
    const MockHttpHandler &handler;

    void doWrite()
    {
        http::async_write(stream, res,
                          [self = shared_from_this()](beast::error_code, std::size_t)
                          {
                              beast::error_code ignored;
                              self->stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
                          });
    }

    void onRead(beast::error_code ec, std::size_t)
    {
        if (ec) return;

        const MockHttpResponse mock = handler(req);
        res = http::response<http::string_body>{mock.status, req.version()};
        res.set(http::field::content_type, "application/json");
        res.keep_alive(false);
        res.body() = mock.body;
        res.prepare_payload();

        if (mock.delayMs > 0)
        {
            timer.expires_after(std::chrono::milliseconds(mock.delayMs));
            timer.async_wait(
                [self = shared_from_this()](beast::error_code timerEc)
                {
                    if (!timerEc) self->doWrite();
                });
        }
        else
        {
            doWrite();
        }
    }

public:
    MockHttpSession(tcp::socket socket, const MockHttpHandler &handler)
        : stream(std::move(socket)), timer(stream.get_executor()), handler(handler)
    {
    }

    void start()
    {
        http::async_read(stream, buffer, req, [self = shared_from_this()](beast::error_code ec, std::size_t bytes)
                         { self->onRead(ec, bytes); });
    }
};

class MockHttpServer
{
private:
    MockHttpHandler handler;
    net::io_context ioc{1};
    tcp::acceptor acceptor;
    std::atomic<bool> stopped{false};
    uint16_t boundPort = 0;
    std::thread serverThread;

    void doAccept()
    {
        acceptor.async_accept(
            [this](beast::error_code ec, tcp::socket socket)
            {
                if (!ec)
                {
                    std::make_shared<MockHttpSession>(std::move(socket), handler)->start();
                }
                if (ec != net::error::operation_aborted && !stopped.load(std::memory_order_acquire))
                {
                    doAccept();
                }
            });
    }

public:
    explicit MockHttpServer(MockHttpHandler handlerFn) : handler(std::move(handlerFn)), acceptor(ioc)
    {
        const tcp::endpoint endpoint(net::ip::address_v4::loopback(), 0);
        acceptor.open(endpoint.protocol());
        acceptor.set_option(net::socket_base::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen(net::socket_base::max_listen_connections);
        boundPort = acceptor.local_endpoint().port();

        doAccept();
        serverThread = std::thread([this]() { ioc.run(); });
    }

    ~MockHttpServer() { stop(); }

    MockHttpServer(const MockHttpServer &) = delete;
    MockHttpServer &operator=(const MockHttpServer &) = delete;

    uint16_t port() const { return boundPort; }

    void stop()
    {
        if (stopped.exchange(true, std::memory_order_acq_rel)) return;
        net::post(ioc,
                  [this]()
                  {
                      beast::error_code ec;
                      acceptor.close(ec);
                  });
        ioc.stop();
        if (serverThread.joinable())
        {
            serverThread.join();
        }
    }
};

class TempDatasetGuard
{
private:
    std::filesystem::path dirPath;

public:
    TempDatasetGuard()
    {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        dirPath = std::filesystem::temp_directory_path() /
                  ("cse_ws_test_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(dirPath);
    }

    ~TempDatasetGuard()
    {
        std::error_code ec;
        std::filesystem::remove_all(dirPath, ec);
    }

    const std::filesystem::path &path() const { return dirPath; }
};

void writeFile(const std::filesystem::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.is_open());
    out << content;
}

json readWsJson(websocket::stream<tcp::socket> &ws)
{
    beast::flat_buffer buf;
    ws.read(buf);
    return json::parse(beast::buffers_to_string(buf.data()));
}

} // namespace

TEST(OllamaAndWebSocketTest, OllamaHealthChecksModelPresenceAndErrors)
{
    const Tokenizer tokenizer;

    {
        MockHttpServer mock([](const http::request<http::string_body> &)
                            { return MockHttpResponse{http::status::ok, R"({"models":[{"name":"qwen2.5:0.5b"}]})"}; });
        OllamaService ollama("127.0.0.1", std::to_string(mock.port()), "qwen2.5:0.5b", tokenizer, 2000);
        EXPECT_TRUE(ollama.checkHealth(std::chrono::milliseconds(1000)));
    }

    {
        MockHttpServer mock([](const http::request<http::string_body> &)
                            { return MockHttpResponse{http::status::ok, R"({"models":[{"name":"llama3:latest"}]})"}; });
        OllamaService ollama("127.0.0.1", std::to_string(mock.port()), "llama3", tokenizer, 2000);
        EXPECT_TRUE(ollama.checkHealth(std::chrono::milliseconds(1000)));
    }

    {
        MockHttpServer mock([](const http::request<http::string_body> &)
                            { return MockHttpResponse{http::status::ok, R"({"models":[{"name":"other:1b"}]})"}; });
        OllamaService ollama("127.0.0.1", std::to_string(mock.port()), "qwen2.5:0.5b", tokenizer, 2000);
        EXPECT_FALSE(ollama.checkHealth(std::chrono::milliseconds(1000)));
    }

    {
        MockHttpServer mock([](const http::request<http::string_body> &)
                            { return MockHttpResponse{http::status::internal_server_error, "error"}; });
        OllamaService ollama("127.0.0.1", std::to_string(mock.port()), "qwen2.5:0.5b", tokenizer, 2000);
        EXPECT_FALSE(ollama.checkHealth(std::chrono::milliseconds(1000)));
    }
}

TEST(OllamaAndWebSocketTest, OllamaExpandQueryNormalizesDeduplicatesCapsAndSerializes)
{
    std::atomic<int> inFlight{0};
    std::atomic<int> maxConcurrent{0};
    std::atomic<int> calls{0};

    MockHttpServer mock(
        [&](const http::request<http::string_body> &req)
        {
            const int active = inFlight.fetch_add(1, std::memory_order_acq_rel) + 1;
            int prevMax = maxConcurrent.load(std::memory_order_acquire);
            while (active > prevMax && !maxConcurrent.compare_exchange_weak(prevMax, active))
            {
            }

            const int callIdx = calls.fetch_add(1, std::memory_order_relaxed);
            std::string body;
            if (callIdx == 0)
            {
                const json inner = {
                    {"terms",
                     {"Spaceship", "ROCKET", "the", "stars", "a", "nebula", "galaxy", "planet", "orbit", "cosmos",
                      "comet", "meteor", "asteroid", "lunar", "solar"}},
                };
                body = json{{"response", inner.dump()}}.dump();
            }
            else
            {
                body = json{{"response", R"(["supernova","pulsar","quasar"])"}}.dump();
            }
            (void)req;
            inFlight.fetch_sub(1, std::memory_order_acq_rel);
            return MockHttpResponse{http::status::ok, std::move(body), 15};
        });

    const Tokenizer tokenizer;
    OllamaService ollama("127.0.0.1", std::to_string(mock.port()), "qwen2.5:0.5b", tokenizer, 3000);

    auto f1 = ollama.expandQuery("rocket stars");
    auto f2 = ollama.expandQuery("astronomy");
    auto f3 = ollama.expandQuery("telescope");

    const ExpansionResult r1 = f1.get();
    const ExpansionResult r2 = f2.get();
    const ExpansionResult r3 = f3.get();

    EXPECT_TRUE(r1.error.empty());
    EXPECT_EQ(r1.expandedTerms.size(), 10u);
    EXPECT_EQ(r1.expandedTerms[0], "spaceship");
    for (const auto &term : r1.expandedTerms)
    {
        EXPECT_NE(term, "rocket");
        EXPECT_NE(term, "stars");
        EXPECT_NE(term, "the");
        EXPECT_GE(term.size(), 2u);
    }

    EXPECT_TRUE(r2.error.empty());
    ASSERT_EQ(r2.expandedTerms.size(), 3u);
    EXPECT_EQ(r2.expandedTerms[0], "supernova");
    EXPECT_TRUE(r3.error.empty());
    EXPECT_EQ(maxConcurrent.load(), 1);
}

TEST(OllamaAndWebSocketTest, OllamaExpandQueryFallbackOnBadJsonHttpErrorAndTimeout)
{
    const Tokenizer tokenizer;

    {
        MockHttpServer mock([](const http::request<http::string_body> &)
                            { return MockHttpResponse{http::status::ok, R"({"response":"not-valid-json"})"}; });
        OllamaService ollama("127.0.0.1", std::to_string(mock.port()), "qwen2.5:0.5b", tokenizer, 2000);
        const ExpansionResult res = ollama.expandQuery("cosmos").get();
        EXPECT_TRUE(res.expandedTerms.empty());
        EXPECT_FALSE(res.error.empty());
    }

    {
        MockHttpServer mock([](const http::request<http::string_body> &)
                            { return MockHttpResponse{http::status::internal_server_error, "boom"}; });
        OllamaService ollama("127.0.0.1", std::to_string(mock.port()), "qwen2.5:0.5b", tokenizer, 2000);
        const ExpansionResult res = ollama.expandQuery("cosmos").get();
        EXPECT_TRUE(res.expandedTerms.empty());
        EXPECT_FALSE(res.error.empty());
    }

    {
        const std::string slowBody = R"({"response":"{\"terms\":[\"star\"]}"})";
        MockHttpServer mock([&slowBody](const http::request<http::string_body> &)
                            { return MockHttpResponse{http::status::ok, slowBody, 500}; });
        OllamaService ollama("127.0.0.1", std::to_string(mock.port()), "qwen2.5:0.5b", tokenizer, 80);
        const ExpansionResult res = ollama.expandQuery("cosmos").get();
        EXPECT_TRUE(res.expandedTerms.empty());
        EXPECT_FALSE(res.error.empty());
    }
}

TEST(OllamaAndWebSocketTest, OllamaStopCancelsInFlightRequestQuickly)
{
    std::atomic<bool> notified{false};
    std::promise<void> arrivedPromise;
    auto arrivedFuture = arrivedPromise.get_future();

    MockHttpServer mock(
        [&](const http::request<http::string_body> &)
        {
            if (!notified.exchange(true, std::memory_order_acq_rel))
            {
                arrivedPromise.set_value();
            }
            return MockHttpResponse{http::status::ok, R"({"response":"{\"terms\":[\"star\"]}"})", 10000};
        });

    const Tokenizer tokenizer;
    OllamaService ollama("127.0.0.1", std::to_string(mock.port()), "qwen2.5:0.5b", tokenizer, 15000);

    auto fut = ollama.expandQuery("cosmos");
    ASSERT_EQ(arrivedFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);

    const auto t0 = std::chrono::steady_clock::now();
    ollama.stop(false);
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    EXPECT_LT(elapsedMs, 2000);
    const ExpansionResult res = fut.get();
    EXPECT_TRUE(res.expandedTerms.empty());
    EXPECT_FALSE(res.error.empty());
}

TEST(OllamaAndWebSocketTest, WebSocketStreamsFullPipelineHandlesInvalidAndReusesConnection)
{
    MockHttpServer mock(
        [](const http::request<http::string_body> &)
        {
            const json inner = {{"terms", {"cosmos", "nebula"}}};
            return MockHttpResponse{http::status::ok, json{{"response", inner.dump()}}.dump()};
        });

    TempDatasetGuard dataset;
    writeFile(dataset.path() / "1001.txt", "dante inferno comedy fire underworld");
    writeFile(dataset.path() / "1002.txt", "distant cosmos nebula galaxies stars");

    const Tokenizer tokenizer;
    const IndexerService indexer(tokenizer, dataset.path().string(), 16);
    const SearchEngine engine(tokenizer);
    OllamaService ollama("127.0.0.1", std::to_string(mock.port()), "qwen2.5:0.5b", tokenizer, 3000);
    RequestHandler handler(ollama, indexer, engine);

    HttpRequest indexReq{http::verb::post, "/api/index", 11};
    indexReq.body() = R"({"threads":2})";
    indexReq.prepare_payload();
    ASSERT_EQ(handler.handle(indexReq).result(), http::status::ok);

    SearchServer server(0, 2, handler);
    std::thread serverThread([&server]() { server.start(); });
    ASSERT_TRUE(server.waitUntilReady());

    net::io_context clientIoc;
    tcp::resolver resolver(clientIoc);
    websocket::stream<tcp::socket> ws(clientIoc);
    net::connect(ws.next_layer(), resolver.resolve("127.0.0.1", std::to_string(server.getBoundPort())));
    ws.handshake("127.0.0.1", "/ws/search");

    ws.write(net::buffer(std::string(R"({"query":"inferno","limit":5})")));
    const json msg1 = readWsJson(ws);
    ASSERT_EQ(msg1["stage"], "query_expansion");
    EXPECT_EQ(msg1["data"]["original"], "inferno");
    ASSERT_EQ(msg1["data"]["expanded_terms"].size(), 2u);
    EXPECT_EQ(msg1["data"]["expanded_terms"][0], "cosmos");
    EXPECT_EQ(msg1["data"]["expanded_terms"][1], "nebula");

    const json msg2 = readWsJson(ws);
    ASSERT_EQ(msg2["stage"], "keyword_search");
    ASSERT_EQ(msg2["data"]["results"].size(), 2u);
    EXPECT_EQ(msg2["data"]["results"][0]["doc_id"], "1001_0");
    EXPECT_EQ(msg2["data"]["results"][1]["doc_id"], "1002_0");
    EXPECT_TRUE(msg2["data"]["time_ms"].is_number_integer());

    const json msg3 = readWsJson(ws);
    EXPECT_EQ(msg3["stage"], "done");

    ws.write(net::buffer(std::string("not-json")));
    const json errMsg = readWsJson(ws);
    EXPECT_EQ(errMsg["stage"], "error");
    EXPECT_FALSE(errMsg["data"]["message"].get<std::string>().empty());

    ws.write(net::buffer(std::string(R"({"query":"dante"})")));
    EXPECT_EQ(readWsJson(ws)["stage"], "query_expansion");
    EXPECT_EQ(readWsJson(ws)["stage"], "keyword_search");
    EXPECT_EQ(readWsJson(ws)["stage"], "done");

    beast::error_code closeEc;
    ws.close(websocket::close_code::normal, closeEc);

    server.stop();
    serverThread.join();
}

TEST(OllamaAndWebSocketTest, WebSocketRejectsUnknownPathAndFallsBackWhenOllamaFails)
{
    MockHttpServer mock([](const http::request<http::string_body> &)
                        { return MockHttpResponse{http::status::internal_server_error, "ollama down"}; });

    TempDatasetGuard dataset;
    writeFile(dataset.path() / "1001.txt", "dante inferno fire");

    const Tokenizer tokenizer;
    const IndexerService indexer(tokenizer, dataset.path().string(), 16);
    const SearchEngine engine(tokenizer);
    OllamaService ollama("127.0.0.1", std::to_string(mock.port()), "qwen2.5:0.5b", tokenizer, 2000);
    RequestHandler handler(ollama, indexer, engine);

    HttpRequest indexReq{http::verb::post, "/api/index", 11};
    indexReq.body() = R"({"threads":1})";
    indexReq.prepare_payload();
    ASSERT_EQ(handler.handle(indexReq).result(), http::status::ok);

    SearchServer server(0, 2, handler);
    std::thread serverThread([&server]() { server.start(); });
    ASSERT_TRUE(server.waitUntilReady());

    net::io_context clientIoc;
    tcp::resolver resolver(clientIoc);

    {
        websocket::stream<tcp::socket> badWs(clientIoc);
        net::connect(badWs.next_layer(), resolver.resolve("127.0.0.1", std::to_string(server.getBoundPort())));
        websocket::response_type upgradeResp;
        beast::error_code handshakeEc;
        badWs.handshake(upgradeResp, "127.0.0.1", "/ws/unknown", handshakeEc);
        EXPECT_TRUE(handshakeEc);
        EXPECT_EQ(upgradeResp.result(), http::status::not_found);
    }

    {
        websocket::stream<tcp::socket> ws(clientIoc);
        net::connect(ws.next_layer(), resolver.resolve("127.0.0.1", std::to_string(server.getBoundPort())));
        ws.handshake("127.0.0.1", "/ws/search");

        ws.write(net::buffer(std::string(R"({"query":"inferno"})")));
        const json m1 = readWsJson(ws);
        ASSERT_EQ(m1["stage"], "query_expansion");
        EXPECT_TRUE(m1["data"]["expanded_terms"].empty());
        EXPECT_FALSE(m1["data"]["error"].get<std::string>().empty());

        const json m2 = readWsJson(ws);
        ASSERT_EQ(m2["stage"], "keyword_search");
        ASSERT_EQ(m2["data"]["results"].size(), 1u);
        EXPECT_EQ(m2["data"]["results"][0]["doc_id"], "1001_0");

        EXPECT_EQ(readWsJson(ws)["stage"], "done");

        beast::error_code ec;
        ws.close(websocket::close_code::normal, ec);
    }

    server.stop();
    serverThread.join();
}

TEST(OllamaAndWebSocketTest, ServerShutdownWithInFlightWebSocketOllamaRequestIsTsanClean)
{
    std::atomic<bool> notified{false};
    std::promise<void> arrivedPromise;
    auto arrivedFuture = arrivedPromise.get_future();

    MockHttpServer mock(
        [&](const http::request<http::string_body> &)
        {
            if (!notified.exchange(true, std::memory_order_acq_rel))
            {
                arrivedPromise.set_value();
            }
            return MockHttpResponse{http::status::ok, R"({"response":"{\"terms\":[\"stars\"]}"})", 5000};
        });

    TempDatasetGuard dataset;
    writeFile(dataset.path() / "1001.txt", "dante inferno fire");

    const Tokenizer tokenizer;
    const IndexerService indexer(tokenizer, dataset.path().string(), 16);
    const SearchEngine engine(tokenizer);
    OllamaService ollama("127.0.0.1", std::to_string(mock.port()), "qwen2.5:0.5b", tokenizer, 10000);
    RequestHandler handler(ollama, indexer, engine);

    SearchServer server(0, 2, handler);
    std::thread serverThread([&server]() { server.start(); });
    ASSERT_TRUE(server.waitUntilReady());

    net::io_context clientIoc;
    tcp::resolver resolver(clientIoc);
    websocket::stream<tcp::socket> ws(clientIoc);
    net::connect(ws.next_layer(), resolver.resolve("127.0.0.1", std::to_string(server.getBoundPort())));
    ws.handshake("127.0.0.1", "/ws/search");

    ws.write(net::buffer(std::string(R"({"query":"inferno"})")));
    ASSERT_EQ(arrivedFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);

    const auto t0 = std::chrono::steady_clock::now();
    server.stop();
    serverThread.join();
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    EXPECT_LT(elapsedMs, 3000);

    beast::error_code ignored;
    ws.next_layer().close(ignored);
}
