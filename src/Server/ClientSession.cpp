#include "ClientSession.h"
#include <chrono>
#include <iostream>
#include <utility>

namespace
{

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

constexpr std::chrono::seconds IDLE_TIMEOUT{ 30 };

bool isBenignError(const beast::error_code& ec)
{
    return ec == beast::error::timeout || ec == net::error::operation_aborted ||
           ec == net::error::connection_reset || ec == http::error::end_of_stream;
}

} // namespace

ClientSession::ClientSession(tcp::socket&& socket, RequestHandler& requestHandler)
    : stream(std::move(socket)), requestHandler(requestHandler)
{
}

void ClientSession::start()
{
    net::dispatch(stream.get_executor(), beast::bind_front_handler(&ClientSession::doRead, shared_from_this()));
}

void ClientSession::doRead()
{
    request = {};
    stream.expires_after(IDLE_TIMEOUT);
    http::async_read(stream, buffer, request, beast::bind_front_handler(&ClientSession::onRead, shared_from_this()));
}

void ClientSession::onRead(beast::error_code ec, std::size_t /*bytesTransferred*/)
{
    if (ec == http::error::end_of_stream)
    {
        return doClose();
    }
    if (ec)
    {
        if (!isBenignError(ec))
        {
            std::cerr << "Read error: " << ec.message() << std::endl;
        }
        return;
    }

    response = requestHandler.handle(request);
    const bool keepAlive = response.keep_alive();
    http::async_write(stream, response,
                      beast::bind_front_handler(&ClientSession::onWrite, shared_from_this(), keepAlive));
}

void ClientSession::onWrite(bool keepAlive, beast::error_code ec, std::size_t /*bytesTransferred*/)
{
    if (ec)
    {
        if (!isBenignError(ec))
        {
            std::cerr << "Write error: " << ec.message() << std::endl;
        }
        return;
    }
    if (!keepAlive)
    {
        return doClose();
    }
    doRead();
}

void ClientSession::doClose()
{
    beast::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
}
