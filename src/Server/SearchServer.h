#ifndef SEARCHSERVER_H
#define SEARCHSERVER_H

#include "RequestHandler.h"
#include "ThreadPool/ThreadPool.h"
#include <atomic>
#include <boost/asio.hpp>
#include <cstdint>
#include <memory>

class SearchServer
{
private:
    uint16_t port;
    int serverThreads;
    RequestHandler &requestHandler;
    boost::asio::io_context ioc;
    boost::asio::signal_set signals;
    std::unique_ptr<ThreadPool> networkPool;
    std::atomic<bool> isRunning{false};

public:
    SearchServer(uint16_t port, int serverThreads, RequestHandler &requestHandler);
    ~SearchServer();

    SearchServer(const SearchServer &) = delete;
    SearchServer &operator=(const SearchServer &) = delete;

    void start();
    void stop();
};

#endif // SEARCHSERVER_H
