#ifndef CSE_REQUEST_HANDLER_H_
#define CSE_REQUEST_HANDLER_H_

#include <boost/beast/http.hpp>

#include "ollama_client.h"

namespace cse {

using HttpRequest =
    boost::beast::http::request<boost::beast::http::string_body>;
using HttpResponse =
    boost::beast::http::response<boost::beast::http::string_body>;

class RequestHandler {
 public:
  explicit RequestHandler(const OllamaClient& ollama);

  HttpResponse Handle(const HttpRequest& request) const;

 private:
  HttpResponse HandleHealth(const HttpRequest& request) const;

  const OllamaClient& ollama_;
};

}  // namespace cse

#endif  // CSE_REQUEST_HANDLER_H_
