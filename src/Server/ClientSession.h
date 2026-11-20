#ifndef CLIENTSESSION_H
#define CLIENTSESSION_H

#include "RequestHandler.h"
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <memory>
#include <string>

class WebSocketSession : public std::enable_shared_from_this<WebSocketSession>
{
private:
    boost::beast::websocket::stream<boost::beast::tcp_stream> ws;
    boost::beast::flat_buffer buffer;
    RequestHandler& requestHandler;
    std::string outgoingMessage;

    void onAccept(boost::beast::error_code ec);
    void doRead();
    void onRead(boost::beast::error_code ec, std::size_t bytesTransferred);
    void sendError(const std::string& message);
    void onExpansionComplete(ExpansionResult expansion, int limit);
    void onStage1Written(ExpansionResult expansion, int limit, boost::beast::error_code ec,
                         std::size_t bytesTransferred);
    void onStage2Written(boost::beast::error_code ec, std::size_t bytesTransferred);
    void onPipelineWritten(boost::beast::error_code ec, std::size_t bytesTransferred);

public:
    WebSocketSession(boost::asio::ip::tcp::socket&& socket, RequestHandler& requestHandler);

    void start(HttpRequest request);
};

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
