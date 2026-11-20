#include "Server/RequestHandler.h"
#include "Index/IndexerService.h"
#include "Index/Tokenizer.h"
#include "Ollama/OllamaService.h"
#include "Search/SearchEngine.h"
#include <atomic>
#include <boost/beast/http.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>

namespace
{

namespace http = boost::beast::http;
using nlohmann::json;

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
                  ("cse_handler_test_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(dirPath);
    }

    ~TempDatasetGuard()
    {
        std::error_code ec;
        std::filesystem::remove_all(dirPath, ec);
    }

    const std::filesystem::path &path() const { return dirPath; }
};

HttpRequest makeRequest(http::verb method, const std::string &target, const std::string &body = "")
{
    HttpRequest req{method, target, 11};
    req.set(http::field::host, "localhost");
    if (!body.empty())
    {
        req.set(http::field::content_type, "application/json");
        req.body() = body;
        req.prepare_payload();
    }
    return req;
}

} // namespace

TEST(RequestHandlerTest, HealthStatsAndEmptySearchBeforeIndex)
{
    TempDatasetGuard dataset;
    const Tokenizer tokenizer;
    const IndexerService indexer(tokenizer, dataset.path().string(), 16);
    const SearchEngine engine(tokenizer);
    OllamaService ollama("127.0.0.1", "59999", "qwen2.5:0.5b", tokenizer, 500);
    RequestHandler handler(ollama, indexer, engine);

    const HttpResponse healthRes = handler.handle(makeRequest(http::verb::get, "/api/health"));
    EXPECT_EQ(healthRes.result(), http::status::ok);
    const json healthBody = json::parse(healthRes.body());
    EXPECT_EQ(healthBody["status"], "ok");
    EXPECT_EQ(healthBody["ollama"], false);

    const HttpResponse statsRes = handler.handle(makeRequest(http::verb::get, "/api/stats"));
    EXPECT_EQ(statsRes.result(), http::status::ok);
    const json statsBody = json::parse(statsRes.body());
    EXPECT_EQ(statsBody["documents"], 0);
    EXPECT_EQ(statsBody["index_build_time_ms"], 0);
    EXPECT_EQ(statsBody["threads_used"], 0);

    const HttpResponse searchRes =
        handler.handle(makeRequest(http::verb::post, "/api/search", R"({"query":"inferno","mode":"keyword"})"));
    EXPECT_EQ(searchRes.result(), http::status::ok);
    const json searchBody = json::parse(searchRes.body());
    EXPECT_TRUE(searchBody["results"].is_array());
    EXPECT_TRUE(searchBody["results"].empty());
    EXPECT_EQ(searchBody["time_ms"], 0);
}

TEST(RequestHandlerTest, ValidatesContractsAndServesSearchAfterIndexing)
{
    TempDatasetGuard dataset;
    {
        std::ofstream f1(dataset.path() / "1001.txt");
        f1 << "Dante inferno comedy circle underworld fire";
        std::ofstream f2(dataset.path() / "1002.txt");
        f2 << "Dante purgatory mountain stars light";
    }

    const Tokenizer tokenizer;
    const IndexerService indexer(tokenizer, dataset.path().string(), 16);
    const SearchEngine engine(tokenizer);
    OllamaService ollama("127.0.0.1", "59999", "qwen2.5:0.5b", tokenizer, 500);
    RequestHandler handler(ollama, indexer, engine);

    // invalid /api/index requests -> 400
    EXPECT_EQ(handler.handle(makeRequest(http::verb::post, "/api/index", "not-json")).result(),
              http::status::bad_request);
    EXPECT_EQ(handler.handle(makeRequest(http::verb::post, "/api/index", R"({"threads":0})")).result(),
              http::status::bad_request);
    EXPECT_EQ(handler.handle(makeRequest(http::verb::post, "/api/index", R"({"threads":65})")).result(),
              http::status::bad_request);
    EXPECT_EQ(handler.handle(makeRequest(http::verb::post, "/api/index", R"({"threads":true})")).result(),
              http::status::bad_request);
    EXPECT_EQ(handler.handle(makeRequest(http::verb::post, "/api/index", R"({"threads":"4"})")).result(),
              http::status::bad_request);

    // valid /api/index -> 200
    const HttpResponse indexRes = handler.handle(makeRequest(http::verb::post, "/api/index", R"({"threads":2})"));
    EXPECT_EQ(indexRes.result(), http::status::ok);
    const json indexBody = json::parse(indexRes.body());
    EXPECT_EQ(indexBody["status"], "ok");
    EXPECT_EQ(indexBody["documents_indexed"], 2);
    EXPECT_TRUE(indexBody["time_ms"].is_number_integer());

    const HttpResponse statsRes = handler.handle(makeRequest(http::verb::get, "/api/stats"));
    const json statsBody = json::parse(statsRes.body());
    EXPECT_EQ(statsBody["documents"], 2);
    EXPECT_EQ(statsBody["threads_used"], 2);

    // invalid /api/search requests -> 400
    EXPECT_EQ(
        handler.handle(makeRequest(http::verb::post, "/api/search", R"({"query":"   ","mode":"keyword"})")).result(),
        http::status::bad_request);
    EXPECT_EQ(
        handler.handle(makeRequest(http::verb::post, "/api/search", R"({"query":"dante","mode":"semantic"})")).result(),
        http::status::bad_request);
    EXPECT_EQ(
        handler
            .handle(makeRequest(http::verb::post, "/api/search", R"({"query":"dante","mode":"keyword","limit":101})"))
            .result(),
        http::status::bad_request);

    // valid /api/search -> 200
    const HttpResponse searchRes =
        handler.handle(makeRequest(http::verb::post, "/api/search", R"({"query":"inferno","mode":"keyword"})"));
    EXPECT_EQ(searchRes.result(), http::status::ok);
    const json searchBody = json::parse(searchRes.body());
    ASSERT_EQ(searchBody["results"].size(), 1u);
    EXPECT_EQ(searchBody["results"][0]["doc_id"], "1001_0");
    EXPECT_DOUBLE_EQ(searchBody["results"][0]["score"].get<double>(), 1.0);
    EXPECT_FALSE(searchBody["results"][0]["snippet"].get<std::string>().empty());

    // stop words only -> 200 empty results
    const HttpResponse stopOnlyRes =
        handler.handle(makeRequest(http::verb::post, "/api/search", R"({"query":"the and of","mode":"keyword"})"));
    EXPECT_EQ(stopOnlyRes.result(), http::status::ok);
    EXPECT_TRUE(json::parse(stopOnlyRes.body())["results"].empty());

    // 405 & 404 checks
    EXPECT_EQ(handler.handle(makeRequest(http::verb::get, "/api/index")).result(), http::status::method_not_allowed);
    EXPECT_EQ(handler.handle(makeRequest(http::verb::get, "/api/unknown")).result(), http::status::not_found);
}
