#include "ThreadPool.h"

ThreadPool::ThreadPool(int threadCount)
{
    if (threadCount <= 0)
    {
        unsigned int logicalCores = std::thread::hardware_concurrency();
        threadCount = logicalCores > 0 ? static_cast<int>(logicalCores) : 1;
    }

    queue = std::make_unique<TaskQueue<std::function<void()>>>();
    workers.reserve(static_cast<size_t>(threadCount));

    try
    {
        for (int i = 0; i < threadCount; i++)
        {
            workers.emplace_back(&ThreadPool::workerLoop, this);
        }
    }
    catch (...)
    {
        stop(false);
        throw;
    }
}

ThreadPool::~ThreadPool()
{
    stop(true);
}

void ThreadPool::workerLoop()
{
    std::function<void()> task;

    while (true)
    {
        bool hasTask = queue->pop(task);
        if (!hasTask) break;

        if (task)
        {
            task();
            tasksCompleted.fetch_add(1);
        }
    }
}

void ThreadPool::stop(bool finishTasks)
{
    std::lock_guard<std::mutex> lock(stopMtx);
    if (stopFlag.exchange(true, std::memory_order_acq_rel)) return;

    if (queue) queue->stop(!finishTasks);

    for (std::thread &worker : workers)
    {
        if (worker.joinable()) worker.join();
    }
}

void ThreadPool::pause()
{
    if (stopFlag.load(std::memory_order_acquire) || !queue) return;
    queue->pause();
}

void ThreadPool::resume()
{
    if (stopFlag.load(std::memory_order_acquire) || !queue) return;
    queue->resume();
}

bool ThreadPool::isWorking() const
{
    return !stopFlag.load(std::memory_order_acquire);
}

bool ThreadPool::isPaused() const
{
    return queue ? queue->isPaused() : false;
}

int ThreadPool::getQueueSize() const
{
    return queue ? queue->size() : 0;
}

int ThreadPool::getTasksSubmitted() const
{
    return queue ? queue->getPushedCount() : 0;
}

int ThreadPool::getTasksCompleted() const
{
    return tasksCompleted.load();
}

int ThreadPool::getThreadCount() const
{
    return static_cast<int>(workers.size());
}
