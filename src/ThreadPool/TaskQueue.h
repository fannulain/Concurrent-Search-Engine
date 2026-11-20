#ifndef TASKQUEUE_H
#define TASKQUEUE_H

#include <condition_variable>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

template <typename TaskType>
class TaskQueue
{
private:
    std::queue<TaskType> innerQueue;
    mutable std::mutex mtx;
    std::condition_variable condVar;

    bool stopFlag = false;
    bool pauseFlag = false;

public:
    TaskQueue() = default;
    ~TaskQueue() = default;

    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    bool empty() const;
    int size() const;

    template <typename TaskArg>
    bool push(TaskArg&& task);

    template <typename... Args>
    bool emplace(Args&&... args);

    bool pop(TaskType& task);
    void clear();

    std::vector<TaskType> stop(bool terminateInstantly);
    void pause();
    void resume();
};

template <typename TaskType>
bool TaskQueue<TaskType>::empty() const
{
    std::lock_guard<std::mutex> lock(mtx);
    return innerQueue.empty();
}

template <typename TaskType>
int TaskQueue<TaskType>::size() const
{
    std::lock_guard<std::mutex> lock(mtx);
    return static_cast<int>(innerQueue.size());
}

template <typename TaskType>
template <typename TaskArg>
bool TaskQueue<TaskType>::push(TaskArg&& task)
{
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (stopFlag) return false;
        innerQueue.push(std::forward<TaskArg>(task));
    }
    condVar.notify_one();
    return true;
}

template <typename TaskType>
template <typename... Args>
bool TaskQueue<TaskType>::emplace(Args&&... args)
{
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (stopFlag) return false;
        innerQueue.emplace(std::forward<Args>(args)...);
    }
    condVar.notify_one();
    return true;
}

template <typename TaskType>
bool TaskQueue<TaskType>::pop(TaskType& task)
{
    std::unique_lock<std::mutex> lock(mtx);

    condVar.wait(lock, [this]() { return stopFlag || (!pauseFlag && !innerQueue.empty()); });

    if (stopFlag && innerQueue.empty()) return false;

    if (!innerQueue.empty())
    {
        task = std::move(innerQueue.front());
        innerQueue.pop();
        return true;
    }

    return false;
}

template <typename TaskType>
void TaskQueue<TaskType>::clear()
{
    std::queue<TaskType> emptyQueue;
    std::lock_guard<std::mutex> lock(mtx);
    std::swap(innerQueue, emptyQueue);
}

template <typename TaskType>
std::vector<TaskType> TaskQueue<TaskType>::stop(bool terminateInstantly)
{
    std::vector<TaskType> droppedTasks;
    {
        std::lock_guard<std::mutex> lock(mtx);
        stopFlag = true;
        pauseFlag = false;

        if (terminateInstantly)
        {
            while (!innerQueue.empty())
            {
                droppedTasks.push_back(std::move(innerQueue.front()));
                innerQueue.pop();
            }
        }
    }

    // notify outside lock so woken workers do not immediately block on mtx
    condVar.notify_all();
    return droppedTasks;
}

template <typename TaskType>
void TaskQueue<TaskType>::pause()
{
    std::lock_guard<std::mutex> lock(mtx);
    pauseFlag = true;
}

template <typename TaskType>
void TaskQueue<TaskType>::resume()
{
    {
        std::lock_guard<std::mutex> lock(mtx);
        pauseFlag = false;
    }
    condVar.notify_all();
}

#endif // TASKQUEUE_H
