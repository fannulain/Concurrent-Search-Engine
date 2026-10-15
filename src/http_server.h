#ifndef CSE_HTTP_SERVER_H_
#define CSE_HTTP_SERVER_H_

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <memory>

#include "request_handler.h"

namespace cse {

class HttpSession : public std::enable_shared_from_this<HttpSession> {
 public:
  HttpSession(boost::asio::ip::tcp::socket&& socket,
              const RequestHandler& handler);

  void Run();

 private:
  void DoRead();
  void OnRead(boost::beast::error_code ec, std::size_t bytes_transferred);
  void OnWrite(bool keep_alive, boost::beast::error_code ec,
               std::size_t bytes_transferred);
  void DoClose();

  boost::beast::tcp_stream stream_;
  boost::beast::flat_buffer buffer_;
  HttpRequest request_;
  HttpResponse response_;
  const RequestHandler& handler_;
};

class Listener : public std::enable_shared_from_this<Listener> {
 public:
  Listener(boost::asio::io_context& ioc,
           const boost::asio::ip::tcp::endpoint& endpoint,
           const RequestHandler& handler);

  void Run();

 private:
  void DoAccept();
  void OnAccept(boost::beast::error_code ec,
                boost::asio::ip::tcp::socket socket);

  boost::asio::io_context& ioc_;
  boost::asio::ip::tcp::acceptor acceptor_;
  const RequestHandler& handler_;
};

}  // namespace cse

#endif  // CSE_HTTP_SERVER_H_
