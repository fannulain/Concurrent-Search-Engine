#include "ollama_client.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <exception>
#include <nlohmann/json.hpp>
#include <optional>
#include <utility>

namespace cse {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

constexpr char kUserAgent[] = "concurrent-search-engine/0.1";

bool ModelListContains(const std::string& body, const std::string& model) {
  const auto json = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (json.is_discarded() || !json.contains("models") ||
      !json["models"].is_array()) {
    return false;
  }
  for (const auto& entry : json["models"]) {
    if (!entry.is_object()) continue;
    const std::string name = entry.value("name", "");
    if (name == model || name == model + ":latest") {
      return true;
    }
  }
  return false;
}

net::awaitable<bool> FetchHasModel(std::string host, std::string port,
                                   std::string model,
                                   std::chrono::milliseconds timeout) {
  auto executor = co_await net::this_coro::executor;
  tcp::resolver resolver(executor);
  beast::tcp_stream stream(executor);

  const auto endpoints =
      co_await resolver.async_resolve(host, port, net::use_awaitable);
  stream.expires_after(timeout);
  co_await stream.async_connect(endpoints, net::use_awaitable);

  http::request<http::empty_body> request{http::verb::get, "/api/tags", 11};
  request.set(http::field::host, host);
  request.set(http::field::user_agent, kUserAgent);
  co_await http::async_write(stream, request, net::use_awaitable);

  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  co_await http::async_read(stream, buffer, response, net::use_awaitable);

  beast::error_code ignored;
  stream.socket().shutdown(tcp::socket::shutdown_both, ignored);

  co_return response.result() == http::status::ok &&
      ModelListContains(response.body(), model);
}

}  // namespace

OllamaClient::OllamaClient(std::string host, std::string port,
                           std::string model)
    : host_(std::move(host)), port_(std::move(port)), model_(std::move(model)) {}

bool OllamaClient::CheckHealth(std::chrono::milliseconds timeout) const {
  net::io_context ioc;
  std::optional<bool> result;
  net::co_spawn(ioc, FetchHasModel(host_, port_, model_, timeout),
                [&result](std::exception_ptr error, bool has_model) {
                  result = !error && has_model;
                });
  ioc.run_for(timeout);
  return result.value_or(false);
}

}  // namespace cse
