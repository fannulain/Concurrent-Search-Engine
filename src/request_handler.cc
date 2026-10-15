#include "request_handler.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string_view>

namespace cse {
namespace {

namespace http = boost::beast::http;
using nlohmann::json;

constexpr std::chrono::milliseconds kOllamaHealthTimeout{2000};

HttpResponse MakeJsonResponse(const HttpRequest& request, http::status status,
                              const json& body) {
  HttpResponse response{status, request.version()};
  response.set(http::field::server, "concurrent-search-engine");
  response.set(http::field::content_type, "application/json");
  response.keep_alive(request.keep_alive());
  response.body() = body.dump();
  response.prepare_payload();
  return response;
}

HttpResponse MakeError(const HttpRequest& request, http::status status,
                       std::string_view message) {
  return MakeJsonResponse(request, status, json{{"error", message}});
}

// Strips the query string: "/api/health?x=1" -> "/api/health".
std::string_view PathOf(const HttpRequest& request) {
  const std::string_view target(request.target().data(),
                                request.target().size());
  return target.substr(0, target.find('?'));
}

}  // namespace

RequestHandler::RequestHandler(const OllamaClient& ollama) : ollama_(ollama) {}

HttpResponse RequestHandler::Handle(const HttpRequest& request) const {
  try {
    const std::string_view path = PathOf(request);
    const http::verb method = request.method();

    if (path == "/api/health") {
      if (method != http::verb::get) {
        return MakeError(request, http::status::method_not_allowed,
                         "Method not allowed");
      }
      return HandleHealth(request);
    }

    if ((path == "/api/index" && method == http::verb::post) ||
        (path == "/api/search" && method == http::verb::post) ||
        (path == "/api/stats" && method == http::verb::get)) {
      return MakeError(request, http::status::not_implemented,
                       "Not implemented yet");
    }

    return MakeError(request, http::status::not_found, "Not found");
  } catch (const std::exception& e) {
    std::cerr << "Request handling failed: " << e.what() << std::endl;
    return MakeError(request, http::status::internal_server_error,
                     "Internal server error");
  }
}

HttpResponse RequestHandler::HandleHealth(const HttpRequest& request) const {
  const bool ollama_ok = ollama_.CheckHealth(kOllamaHealthTimeout);
  return MakeJsonResponse(request, http::status::ok,
                          json{{"status", "ok"}, {"ollama", ollama_ok}});
}

}  // namespace cse
