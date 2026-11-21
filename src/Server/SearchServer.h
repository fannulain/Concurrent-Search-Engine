#ifndef SEARCHSERVER_H
#define SEARCHSERVER_H

#include "RequestHandler.h"
#include "ThreadPool/ThreadPool.h"
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

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
    std::atomic<uint16_t> boundPort{0};
    mutable std::mutex readyMtx;
    std::condition_variable readyCv;
    bool ready = false;

public:
    SearchServer(uint16_t port, int serverThreads, RequestHandler &requestHandler);
    ~SearchServer();

    SearchServer(const SearchServer &) = delete;
    SearchServer &operator=(const SearchServer &) = delete;

    void start();
    void stop();
    bool waitUntilReady(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
    uint16_t getBoundPort() const;
};

#endif // SEARCHSERVER_H
