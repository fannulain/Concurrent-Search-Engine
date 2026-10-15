#include "http_server.h"

#include <chrono>
#include <iostream>
#include <utility>

namespace cse {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

constexpr std::chrono::seconds kIdleTimeout{30};

bool IsBenignError(const beast::error_code& ec) {
  return ec == beast::error::timeout || ec == net::error::operation_aborted ||
         ec == net::error::connection_reset || ec == http::error::end_of_stream;
}

}  // namespace

HttpSession::HttpSession(tcp::socket&& socket, const RequestHandler& handler)
    : stream_(std::move(socket)), handler_(handler) {}

void HttpSession::Run() {
  net::dispatch(stream_.get_executor(),
                beast::bind_front_handler(&HttpSession::DoRead,
                                          shared_from_this()));
}

void HttpSession::DoRead() {
  request_ = {};
  stream_.expires_after(kIdleTimeout);
  http::async_read(stream_, buffer_, request_,
                   beast::bind_front_handler(&HttpSession::OnRead,
                                             shared_from_this()));
}

void HttpSession::OnRead(beast::error_code ec, std::size_t /*bytes*/) {
  if (ec == http::error::end_of_stream) {
    return DoClose();
  }
  if (ec) {
    if (!IsBenignError(ec)) {
      std::cerr << "read: " << ec.message() << std::endl;
    }
    return;
  }

  response_ = handler_.Handle(request_);
  const bool keep_alive = response_.keep_alive();
  http::async_write(stream_, response_,
                    beast::bind_front_handler(&HttpSession::OnWrite,
                                              shared_from_this(), keep_alive));
}

void HttpSession::OnWrite(bool keep_alive, beast::error_code ec,
                          std::size_t /*bytes*/) {
  if (ec) {
    if (!IsBenignError(ec)) {
      std::cerr << "write: " << ec.message() << std::endl;
    }
    return;
  }
  if (!keep_alive) {
    return DoClose();
  }
  DoRead();
}

void HttpSession::DoClose() {
  beast::error_code ignored;
  stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
}

Listener::Listener(net::io_context& ioc, const tcp::endpoint& endpoint,
                   const RequestHandler& handler)
    : ioc_(ioc), acceptor_(net::make_strand(ioc)), handler_(handler) {
  acceptor_.open(endpoint.protocol());
  acceptor_.set_option(net::socket_base::reuse_address(true));
  acceptor_.bind(endpoint);
  acceptor_.listen(net::socket_base::max_listen_connections);
}

void Listener::Run() { DoAccept(); }

void Listener::DoAccept() {
  acceptor_.async_accept(
      net::make_strand(ioc_),
      beast::bind_front_handler(&Listener::OnAccept, shared_from_this()));
}

void Listener::OnAccept(beast::error_code ec, tcp::socket socket) {
  if (ec == net::error::operation_aborted) {
    return;
  }
  if (ec) {
    std::cerr << "accept: " << ec.message() << std::endl;
  } else {
    std::make_shared<HttpSession>(std::move(socket), handler_)->Run();
  }
  DoAccept();
}

}  // namespace cse
