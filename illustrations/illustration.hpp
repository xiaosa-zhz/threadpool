/*
    * This file provides a simplified demonstration of how concurrent_queue works
*/

#include <cstddef>
#include <atomic>
#include <memory>
#include <thread>
#include <array>
#include <print>

struct buffer {
    alignas(64) std::atomic_size_t local_counter = 0;
    int buffer_placeholder[1] = {};
};

struct buffers {
    alignas(64) std::atomic_uint64_t shared_counter = 0;
    std::atomic_flag steal_flag = false;
    std::array<std::unique_ptr<buffer>, 3> bs = {
        std::make_unique<buffer>(),
        std::make_unique<buffer>(),
        std::make_unique<buffer>()
    };

    constexpr static std::size_t mask = ~((~0uz) >> 1);

    buffer* read() noexcept {
        while (steal_flag.test_and_set(std::memory_order_acquire)) {
            std::println("wait for thief to finish");
            std::this_thread::yield();
        }
        
        const auto curr = mask & shared_counter.load(std::memory_order_relaxed);
        const auto next = mask ^ curr;
        auto prev_counter = (~mask) & shared_counter.exchange(next, std::memory_order_relaxed);
        auto& buffer_handle = bs[curr ? 1 : 0];
        while (buffer_handle->local_counter.load(std::memory_order_acquire) != prev_counter) {
            std::println("wait for local_counter to match shared_counter");
            std::this_thread::yield();
        }
        auto& local_handle = bs[2];
        std::ranges::swap(local_handle, buffer_handle);

        steal_flag.clear(std::memory_order_release);
        return local_handle.get();
    }

    buffer* steal(buffers& other) noexcept {
        if (other.steal_flag.test_and_set(std::memory_order_acquire)) {
            return nullptr;
        }

        const auto other_curr = mask & other.shared_counter.load(std::memory_order_relaxed);
        const auto other_next = mask ^ other_curr;
        auto prev_counter = (~mask) & other.shared_counter.exchange(other_next, std::memory_order_relaxed);
        auto& buffer_handle = other.bs[other_curr ? 1 : 0];
        while (buffer_handle->local_counter.load(std::memory_order_acquire) != prev_counter) {
            std::println("wait for local_counter to match shared_counter in steal");
            std::this_thread::yield();
        }
        auto& local_handle = bs[2];
        std::ranges::swap(local_handle, buffer_handle);

        other.steal_flag.clear(std::memory_order_release);
        return local_handle.get();
    }

    bool write() noexcept {
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
