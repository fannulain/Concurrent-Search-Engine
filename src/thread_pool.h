#ifndef CSE_THREAD_POOL_H_
#define CSE_THREAD_POOL_H_

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace cse {

class ThreadPool {
 public:
  explicit ThreadPool(std::size_t num_threads);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  template <typename F>
  auto Enqueue(F&& task) -> std::future<std::invoke_result_t<F>> {
    using Result = std::invoke_result_t<F>;
    auto packaged =
        std::make_shared<std::packaged_task<Result()>>(std::forward<F>(task));
    std::future<Result> result = packaged->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) {
        throw std::runtime_error("Enqueue on a stopped ThreadPool");
      }
      tasks_.emplace([packaged] { (*packaged)(); });
    }
    condition_.notify_one();
    return result;
  }

  std::size_t Size() const { return workers_.size(); }

 private:
  void WorkerLoop();

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool stop_ = false;
};

}  // namespace cse

#endif  // CSE_THREAD_POOL_H_
