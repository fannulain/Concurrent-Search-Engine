#ifndef REQUESTHANDLER_H
#define REQUESTHANDLER_H

#include "Index/IndexSnapshot.h"
#include "Index/IndexerService.h"
#include "Ollama/OllamaService.h"
#include "Search/SearchEngine.h"
#include <atomic>
#include <boost/beast/http.hpp>
#include <memory>

using HttpRequest = boost::beast::http::request<boost::beast::http::string_body>;
using HttpResponse = boost::beast::http::response<boost::beast::http::string_body>;

class RequestHandler
{
private:
    OllamaService &ollamaService;
    const IndexerService &indexerService;
    const SearchEngine &searchEngine;

    std::atomic<std::shared_ptr<const IndexSnapshot>> activeSnapshot;
    std::atomic<bool> buildInProgress{false};

    HttpResponse handleHealth(const HttpRequest &request) const;
    HttpResponse handleIndex(const HttpRequest &request);
    HttpResponse handleSearch(const HttpRequest &request) const;
    HttpResponse handleStats(const HttpRequest &request) const;

public:
    RequestHandler(OllamaService &ollamaService, const IndexerService &indexerService,
                   const SearchEngine &searchEngine);

    HttpResponse handle(const HttpRequest &request);

    std::shared_ptr<const IndexSnapshot> getSnapshot() const;
    OllamaService &getOllamaService();
    const SearchEngine &getSearchEngine() const;
};

#endif // REQUESTHANDLER_H
