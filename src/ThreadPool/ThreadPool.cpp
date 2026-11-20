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

    for (int i = 0; i < threadCount; i++)
    {
        workers.emplace_back(&ThreadPool::workerLoop, this);
    }
}

ThreadPool::~ThreadPool()
{
    if (!stopFlag.load()) stop(true);
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
    if (stopFlag.exchange(true)) return;

    queue->stop(!finishTasks);

    for (std::thread& worker : workers)
    {
        if (worker.joinable()) worker.join();
    }
}

void ThreadPool::pause()
{
    if (stopFlag.load() || pauseFlag.exchange(true)) return;
    queue->pause();
}

void ThreadPool::resume()
{
    if (stopFlag.load() || !pauseFlag.exchange(false)) return;
    queue->resume();
}

bool ThreadPool::isWorking() const
{
    return !stopFlag.load();
}

bool ThreadPool::isPaused() const
{
    return pauseFlag.load();
}

int ThreadPool::getQueueSize() const
{
    return queue ? queue->size() : 0;
}

int ThreadPool::getTasksSubmitted() const
{
    return tasksSubmitted.load();
}

int ThreadPool::getTasksCompleted() const
{
    return tasksCompleted.load();
}

int ThreadPool::getThreadCount() const
{
    return static_cast<int>(workers.size());
}
