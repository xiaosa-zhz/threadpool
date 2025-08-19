/*
    * This file provides a simplified demonstration of how concurrent_queue works
*/

#include <cstddef>
#include <atomic>
#include <memory>
#include <thread>
#include <print>

struct buffer {
    alignas(64) std::atomic_size_t local_counter = 0;
    int buffer_placeholder[1] = {};
};

struct buffers {
    alignas(64) std::atomic_uint64_t shared_counter = 0;
    std::unique_ptr<buffer> bs[2];

    constexpr static std::size_t mask = ~((~0uz) >> 1);

    buffer& read() {
        const auto curr = mask & shared_counter.load(std::memory_order_relaxed);
        const auto next = mask ^ curr;
        auto prev_counter = (~mask) & shared_counter.exchange(next, std::memory_order_relaxed);
        buffer& bf = *bs[curr ? 1 : 0];
        while (bf.local_counter.load(std::memory_order_acquire) != prev_counter) {
            std::println("wait for local_counter to match shared_counter");
            std::this_thread::yield();
        }
        return bf;
    }

    bool write() {
        const auto curr = mask & shared_counter.fetch_add(1, std::memory_order_relaxed);
        auto& bf = *bs[curr ? 1 : 0];

        // Do write things
        {
            bf.buffer_placeholder[0] = 42; // Example write operation
        }

        (void) bf.local_counter.fetch_add(1, std::memory_order_release);
        return true;
    }
};
