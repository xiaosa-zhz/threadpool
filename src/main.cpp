#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <ranges>
#include <print>

#include "concurrent_queue.hpp"

using job = std::move_only_function<void()>;

// #define MYDEBUG 1

#ifdef MYDEBUG
#define DEBUG_PRINT(...) \
    std::println(__VA_ARGS__)
#else
#define DEBUG_PRINT(...) \
    do {} while (false)
#endif

void test_1() {
    using namespace std::literals;
    mylib::concurrent_queue<job> queue(256);
    std::vector<std::jthread> threads;
    const auto thrd_cnt = std::thread::hardware_concurrency();
    DEBUG_PRINT("Thread count: {}", thrd_cnt);
    threads.reserve(thrd_cnt);
    std::atomic_bool flag = false;
    std::atomic_size_t job_counter = 0;
    for (auto i : std::views::iota(0u, thrd_cnt - 1)) {
        threads.emplace_back([&queue, &flag, i, &job_counter] {
            for (auto j : std::views::iota(0, 256)) {
                while (!flag.load(std::memory_order_relaxed)) {
                    std::this_thread::yield();
                }
                while (!queue.enqueue([i, j, &job_counter] {
                    // DEBUG_PRINT("Job {} from thread {}", j, i);
                    job_counter.fetch_add(1, std::memory_order_relaxed);
                })) {
                    std::this_thread::yield();
                }
            }
        });
    }
    flag.store(true, std::memory_order_relaxed);
    std::size_t counter = 0;
    std::size_t round = 0;
    while (counter < (thrd_cnt - 1) * 256) {
        auto jobs = queue.wait_for_exclusive_values();
        DEBUG_PRINT("Round {} has {} jobs.", round++, jobs.size());
        for (auto& j : jobs) {
            j();
            ++counter;
        }
    }
    if (counter != job_counter.load(std::memory_order_relaxed)) {
        DEBUG_PRINT("Counter mismatch: {} != {}", counter, job_counter.load(std::memory_order_relaxed));
    } else {
        DEBUG_PRINT("All jobs executed successfully.");
    }
}

thread_local std::string_view local_job;

void test_2() {
    using namespace std::literals;
    constexpr static std::size_t queue_capacity = 1024;
    constexpr static std::size_t total_jobs = queue_capacity * 16384;
    mylib::concurrent_queue<job> queue1(queue_capacity), queue2(queue_capacity);
    std::vector<std::jthread> producers;
    const auto thrd_cnt = std::thread::hardware_concurrency();
    DEBUG_PRINT("Thread count: {}", thrd_cnt);
    producers.reserve(thrd_cnt - 2);
    std::atomic_bool flag = false;
    std::atomic_size_t job_counter = 0;
    for (auto i : std::views::iota(0u, thrd_cnt - 2)) {
        producers.emplace_back([&queue1, &queue2, &flag, i, &job_counter] {
            while (!flag.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
            for (auto j : std::views::iota(0uz, total_jobs)) {
                const bool target_queue = j % 3 == 0;
                auto new_job = [i, j, target_queue, &job_counter] {
                    // DEBUG_PRINT("Producer {} enqueued job {} to queue {}, {}", i, j, target_queue ? 1 : 2, local_job);
                    job_counter.fetch_add(1, std::memory_order_relaxed);
                };
                if (target_queue) {
                    while (!queue1.enqueue(std::move(new_job))) {
                        std::this_thread::yield();
                    }
                } else {
                    while (!queue2.enqueue(std::move(new_job))) {
                        std::this_thread::yield();
                    }
                }
            }
        });
    }
    std::atomic_bool consumer_flag = false;
    std::size_t c1 = 0, c2 = 0;
    std::size_t cs1 = 0, cs2 = 0;
    std::jthread consumer1([&c1, &cs1, &queue1, &queue2, &job_counter, &consumer_flag, thrd_cnt] {
        local_job = "Consumer 1 executed the job";
        std::size_t round = 0;

        while (!consumer_flag.load(std::memory_order_relaxed)) {
            std::this_thread::yield();
        }
        DEBUG_PRINT("Consumer 1 started.");

        while (job_counter.load(std::memory_order_relaxed) < (thrd_cnt - 2) * total_jobs) {
            auto jobs = queue1.wait_for_exclusive_values();
            DEBUG_PRINT("Consumer 1 Round {} has {} jobs.", round++, jobs.size());
            if (jobs.empty()) {
                DEBUG_PRINT("Consumer 1 found no jobs, stealing from queue 2.");
                jobs = queue1.steal(queue2);
                DEBUG_PRINT("Consumer 1 stole {} jobs from queue 2.", jobs.size());
                cs1 += jobs.size();
                if (jobs.empty()) {
                    std::this_thread::yield();
                    continue;
                }
            }
            c1 += jobs.size();
            for (auto& j : jobs) {
                j();
            }
        }
    });
    std::jthread consumer2([&c2, &cs2, &queue1, &queue2, &job_counter, &consumer_flag, thrd_cnt] {
        local_job = "Consumer 2 executed the job";
        std::size_t round = 0;

        while (!consumer_flag.load(std::memory_order_relaxed)) {
            std::this_thread::yield();
        }
        DEBUG_PRINT("Consumer 2 started.");

        while (job_counter.load(std::memory_order_relaxed) < (thrd_cnt - 2) * total_jobs) {
            auto jobs = queue2.wait_for_exclusive_values();
            DEBUG_PRINT("Consumer 2 Round {} has {} jobs.", round++, jobs.size());
            if (jobs.empty()) {
                DEBUG_PRINT("Consumer 2 found no jobs, stealing from queue 1.");
                jobs = queue2.steal(queue1);
                DEBUG_PRINT("Consumer 2 stole {} jobs from queue 1.", jobs.size());
                cs2 += jobs.size();
                if (jobs.empty()) {
                    std::this_thread::yield();
                    continue;
                }
            }
            c2 += jobs.size();
            for (auto& j : jobs) {
                j();
            }
        }
    });
    flag.store(true, std::memory_order_relaxed);
    consumer_flag.store(true, std::memory_order_relaxed);
    producers.clear();
    consumer1.join();
    consumer2.join();
    std::println("Total jobs processed: {}", job_counter.load(std::memory_order_relaxed));
    if (c1 + c2 != job_counter.load(std::memory_order_relaxed)) {
        std::println("Counter mismatch: {} + {} != {}", c1, c2, job_counter.load(std::memory_order_relaxed));
    } else {
        std::println("All jobs executed successfully.");
    }
    std::println("Consumer 1 processed {} jobs, Consumer 2 processed {} jobs.", c1, c2);
    std::println("Consumer 1 stole {} jobs, Consumer 2 stole {} jobs.", cs1, cs2);
}

int main() {
    test_2();
}
