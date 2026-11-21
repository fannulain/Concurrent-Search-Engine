#include "ThreadPool/ThreadPool.h"
#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>
#include <vector>

TEST(ThreadPoolTest, ReturnsFutureValuesAndPropagatesExceptions)
{
    ThreadPool pool(2);

    std::future<int> sumFuture = pool.addTask([](int a, int b) { return a + b; }, 19, 23);
    EXPECT_EQ(sumFuture.get(), 42);

    std::future<void> errorFuture = pool.addTask([]() { throw std::runtime_error("task failure"); });
    EXPECT_THROW(errorFuture.get(), std::runtime_error);
}

TEST(ThreadPoolTest, GracefulShutdownCompletesQueuedTasks)
{
    std::atomic<int> counter{0};
    {
        ThreadPool pool(4);
        for (int i = 0; i < 100; i++)
        {
            pool.addTask([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
        }
        pool.stop(true);
        EXPECT_EQ(pool.getTasksSubmitted(), 100);
        EXPECT_EQ(pool.getTasksCompleted(), 100);
    }
    EXPECT_EQ(counter.load(), 100);
}

TEST(ThreadPoolTest, PauseAndResumeControlExecution)
{
    ThreadPool pool(2);
    pool.pause();
    EXPECT_TRUE(pool.isPaused());

    std::atomic<int> executed{0};
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 10; i++)
    {
        futures.push_back(pool.addTask([&executed]() { executed.fetch_add(1, std::memory_order_relaxed); }));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_EQ(executed.load(), 0);

    pool.resume();
    EXPECT_FALSE(pool.isPaused());

    for (auto &fut : futures)
    {
        fut.get();
    }
    EXPECT_EQ(executed.load(), 10);
}

TEST(ThreadPoolTest, AddTaskAfterStopThrows)
{
    ThreadPool pool(2);
    pool.stop(true);
    EXPECT_FALSE(pool.isWorking());
    EXPECT_THROW(pool.addTask([]() { return 1; }), std::runtime_error);
}

TEST(ThreadPoolTest, ConcurrentProducersStressTest)
{
    ThreadPool pool(4);
    std::atomic<int> total{0};
    std::vector<std::thread> producers;

    for (int p = 0; p < 8; p++)
    {
        producers.emplace_back(
            [&pool, &total]()
            {
                std::vector<std::future<void>> localFutures;
                for (int i = 0; i < 200; i++)
                {
                    localFutures.push_back(pool.addTask([&total]() { total.fetch_add(1, std::memory_order_relaxed); }));
                }
                for (auto &fut : localFutures)
                {
                    fut.get();
                }
            });
    }

    for (auto &producer : producers)
    {
        producer.join();
    }

    pool.stop(true);
    EXPECT_EQ(total.load(), 1600);
    EXPECT_EQ(pool.getTasksSubmitted(), 1600);
    EXPECT_EQ(pool.getTasksCompleted(), 1600);
}

TEST(ThreadPoolTest, ConcurrentPauseAndResumeDoesNotDeadlock)
{
    ThreadPool pool(4);
    std::atomic<bool> stopToggling{false};
    std::atomic<int> executed{0};

    std::vector<std::thread> togglers;
    for (int t = 0; t < 2; t++)
    {
        togglers.emplace_back(
            [&pool, &stopToggling, t]()
            {
                while (!stopToggling.load(std::memory_order_acquire))
                {
                    if ((t % 2) == 0)
                    {
                        pool.pause();
                    }
                    else
                    {
                        pool.resume();
                    }
                    std::this_thread::yield();
                }
            });
    }

    std::vector<std::future<void>> futures;
    futures.reserve(400);
    for (int i = 0; i < 400; i++)
    {
        futures.push_back(pool.addTask([&executed]() { executed.fetch_add(1, std::memory_order_relaxed); }));
    }

    stopToggling.store(true, std::memory_order_release);
    for (auto &toggler : togglers)
    {
        toggler.join();
    }

    pool.resume();
    EXPECT_FALSE(pool.isPaused());

    for (auto &fut : futures)
    {
        fut.get();
    }

    pool.stop(true);
    EXPECT_EQ(executed.load(), 400);
}

TEST(ThreadPoolTest, ConcurrentStopIsThreadSafe)
{
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    for (int i = 0; i < 200; i++)
    {
        pool.addTask(
            [&counter]()
            {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                counter.fetch_add(1, std::memory_order_relaxed);
            });
    }

    std::vector<std::thread> stoppers;
    for (int s = 0; s < 4; s++)
    {
        stoppers.emplace_back([&pool]() { pool.stop(true); });
    }
    for (auto &stopper : stoppers)
    {
        stopper.join();
    }

    EXPECT_FALSE(pool.isWorking());
    EXPECT_EQ(counter.load(), 200);
    EXPECT_EQ(pool.getTasksCompleted(), 200);
}

TEST(ThreadPoolTest, SubmittedCountNeverLessThanCompleted)
{
    ThreadPool pool(4);
    std::vector<std::thread> producers;

    for (int p = 0; p < 4; p++)
    {
        producers.emplace_back(
            [&pool]()
            {
                std::vector<std::future<void>> futs;
                for (int i = 0; i < 250; i++)
                {
                    futs.push_back(pool.addTask([]() {}));
                    const int completed = pool.getTasksCompleted();
                    const int submitted = pool.getTasksSubmitted();
                    EXPECT_GE(submitted, completed);
                }
                for (auto &f : futs)
                {
                    f.get();
                }
            });
    }

    for (auto &producer : producers)
    {
        producer.join();
    }

    pool.stop(true);
    EXPECT_EQ(pool.getTasksSubmitted(), 1000);
    EXPECT_EQ(pool.getTasksCompleted(), 1000);
}
