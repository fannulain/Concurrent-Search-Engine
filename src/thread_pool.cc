#include "thread_pool.h"

namespace cse {

ThreadPool::ThreadPool(std::size_t num_threads) {
  if (num_threads == 0) {
    throw std::invalid_argument("ThreadPool requires at least one thread");
  }
  workers_.reserve(num_threads);
  for (std::size_t i = 0; i < num_threads; ++i) {
    workers_.emplace_back([this] { WorkerLoop(); });
  }
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  condition_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

void ThreadPool::WorkerLoop() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
      if (stop_ && tasks_.empty()) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    task();
  }
}

}  // namespace cse
