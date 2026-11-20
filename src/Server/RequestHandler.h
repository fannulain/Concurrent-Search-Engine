#ifndef REQUESTHANDLER_H
#define REQUESTHANDLER_H

#include "Ollama/OllamaService.h"
#include <boost/beast/http.hpp>

using HttpRequest = boost::beast::http::request<boost::beast::http::string_body>;
using HttpResponse = boost::beast::http::response<boost::beast::http::string_body>;

class RequestHandler
{
private:
    const OllamaService& ollamaService;

    HttpResponse handleHealth(const HttpRequest& request) const;

public:
    explicit RequestHandler(const OllamaService& ollamaService);

    HttpResponse handle(const HttpRequest& request) const;
};

#endif // REQUESTHANDLER_H
