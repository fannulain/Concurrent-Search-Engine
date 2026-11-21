#include "SearchServer.h"
#include "ClientSession.h"
#include <boost/beast/core.hpp>
#include <csignal>
#include <iostream>
#include <utility>

namespace
{

namespace beast = boost::beast;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class Listener : public std::enable_shared_from_this<Listener>
{
private:
    net::io_context &ioc;
    tcp::acceptor acceptor;
    RequestHandler &requestHandler;

    void doAccept()
    {
        acceptor.async_accept(net::make_strand(ioc),
                              beast::bind_front_handler(&Listener::onAccept, shared_from_this()));
    }

    void onAccept(beast::error_code ec, tcp::socket socket)
    {
        if (ec == net::error::operation_aborted)
        {
            return;
        }
        if (ec)
        {
            std::cerr << "Accept error: " << ec.message() << std::endl;
        }
        else
        {
            std::make_shared<ClientSession>(std::move(socket), requestHandler)->start();
        }
        doAccept();
    }

public:
    Listener(net::io_context &ioc, const tcp::endpoint &endpoint, RequestHandler &requestHandler)
        : ioc(ioc), acceptor(net::make_strand(ioc)), requestHandler(requestHandler)
    {
        acceptor.open(endpoint.protocol());
        acceptor.set_option(net::socket_base::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen(net::socket_base::max_listen_connections);
    }

    void start() { doAccept(); }
    uint16_t localPort() const { return acceptor.local_endpoint().port(); }
};

} // namespace

SearchServer::SearchServer(uint16_t port, int serverThreads, RequestHandler &requestHandler)
    : port(port), serverThreads(serverThreads), requestHandler(requestHandler), ioc(serverThreads),
      signals(ioc, SIGINT, SIGTERM)
{
}

SearchServer::~SearchServer()
{
    stop();
    requestHandler.getOllamaService().stop(false);
}

void SearchServer::start()
{
    if (isRunning.exchange(true)) return;

    auto listener = std::make_shared<Listener>(ioc, tcp::endpoint{net::ip::address_v4::any(), port}, requestHandler);
    const uint16_t actualPort = listener->localPort();
    boundPort.store(actualPort, std::memory_order_release);
    listener->start();

    signals.async_wait(
        [this](const boost::system::error_code &, int signal)
        {
            std::cout << "Received signal " << signal << ", shutting down" << std::endl;
            stop();
        });

    std::cout << "Search server listening on 0.0.0.0:" << actualPort << " with " << serverThreads
              << " network threads" << std::endl;

    networkPool = std::make_unique<ThreadPool>(serverThreads);
    for (int i = 0; i < serverThreads; i++)
    {
        networkPool->addTask([this]() { ioc.run(); });
    }

    {
        std::lock_guard<std::mutex> lock(readyMtx);
        ready = true;
    }
    readyCv.notify_all();

    networkPool->stop(true);
    requestHandler.getOllamaService().stop(false);

    {
        std::lock_guard<std::mutex> lock(readyMtx);
        ready = false;
    }
    readyCv.notify_all();
    std::cout << "Server stopped" << std::endl;
}

void SearchServer::stop()
{
    if (!isRunning.exchange(false)) return;
    ioc.stop();
}

bool SearchServer::waitUntilReady(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(readyMtx);
    return readyCv.wait_for(lock, timeout, [this]() { return ready; });
}

uint16_t SearchServer::getBoundPort() const
{
    return boundPort.load(std::memory_order_acquire);
}
