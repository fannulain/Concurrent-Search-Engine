#ifndef CLIENTSESSION_H
#define CLIENTSESSION_H

#include "RequestHandler.h"
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <memory>

class ClientSession : public std::enable_shared_from_this<ClientSession>
{
private:
    boost::beast::tcp_stream stream;
    boost::beast::flat_buffer buffer;
    HttpRequest request;
    HttpResponse response;
    RequestHandler& requestHandler;

    void doRead();
    void onRead(boost::beast::error_code ec, std::size_t bytesTransferred);
    void onWrite(bool keepAlive, boost::beast::error_code ec, std::size_t bytesTransferred);
    void doClose();

public:
    ClientSession(boost::asio::ip::tcp::socket&& socket, RequestHandler& requestHandler);

    void start();
};

#endif // CLIENTSESSION_H
