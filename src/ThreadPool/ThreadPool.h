#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "TaskQueue.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool
{
private:
    std::unique_ptr<TaskQueue<std::function<void()>>> queue;
    std::vector<std::thread> workers;

    std::atomic<bool> stopFlag{ false };
    std::atomic<bool> pauseFlag{ false };

    std::atomic<int> tasksSubmitted{ 0 };
    std::atomic<int> tasksCompleted{ 0 };

    void workerLoop();

public:
    explicit ThreadPool(int threadCount = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <typename Func, typename... Args>
    auto addTask(Func&& func, Args&&... args) -> std::future<std::invoke_result_t<Func, Args...>>;

    void stop(bool finishTasks = true);
    void pause();
    void resume();

    bool isWorking() const;
    bool isPaused() const;

    int getQueueSize() const;
    int getTasksSubmitted() const;
    int getTasksCompleted() const;
    int getThreadCount() const;
};

template <typename Func, typename... Args>
auto ThreadPool::addTask(Func&& func, Args&&... args) -> std::future<std::invoke_result_t<Func, Args...>>
{
    using ReturnType = std::invoke_result_t<Func, Args...>;

    if (stopFlag.load() || !queue)
    {
        throw std::runtime_error("Cannot add task to a stopped thread pool");
    }

    auto boundTask = [f = std::forward<Func>(func),
                      capturedArgs = std::make_tuple(std::forward<Args>(args)...)]() mutable -> ReturnType
    {
        return std::apply(std::move(f), std::move(capturedArgs));
    };

    auto packagedTask = std::make_shared<std::packaged_task<ReturnType()>>(std::move(boundTask));
    std::future<ReturnType> resultFuture = packagedTask->get_future();

    bool pushed = queue->push([packagedTask]() { (*packagedTask)(); });
    if (!pushed)
    {
        throw std::runtime_error("Cannot add task to a stopped thread pool");
    }

    tasksSubmitted.fetch_add(1);
    return resultFuture;
}

#endif // THREADPOOL_H
