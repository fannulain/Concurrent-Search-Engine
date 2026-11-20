#include "RequestHandler.h"
#include <chrono>
#include <exception>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string_view>

namespace
{

namespace http = boost::beast::http;
using nlohmann::json;

constexpr std::chrono::milliseconds OLLAMA_HEALTH_TIMEOUT{ 2000 };

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

} // namespace

RequestHandler::RequestHandler(const OllamaService& ollamaService) : ollamaService(ollamaService)
{
}

HttpResponse RequestHandler::handle(const HttpRequest& request) const
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

        if ((path == "/api/index" && method == http::verb::post) ||
            (path == "/api/search" && method == http::verb::post) ||
            (path == "/api/stats" && method == http::verb::get))
        {
            return makeError(request, http::status::not_implemented, "Not implemented yet");
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
