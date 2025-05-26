#ifndef MYLIB_THREAD_WORKER_H
#define MYLIB_THREAD_WORKER_H 1

#include <type_traits>
#include <cstddef>
#include <new>
#include <memory>
#include <iterator>
#include <atomic>
#include <span>
#include <thread>

namespace mylib {

    namespace details {

        struct nonmovable {
            nonmovable() = default;
            nonmovable(const nonmovable&) = delete;
            nonmovable(nonmovable&&) = delete;
            nonmovable& operator=(const nonmovable&) = delete;
            nonmovable& operator=(nonmovable&&) = delete;
        };

        template<typename F>
        struct defer {
            ~defer() { this->f(); }
            F f;
        };

        inline constexpr std::size_t queue_align = std::hardware_constructive_interference_size;
        inline constexpr std::size_t queue_align = std::hardware_destructive_interference_size;

    } // namespace mylib::details
    
    template<typename T>
    class alignas(details::queue_align) queue : details::nonmovable
    {
    public:
        using value_type = T;
        using size_type = std::size_t;

        static_assert(std::is_default_constructible_v<value_type>);
        static_assert(std::is_nothrow_move_assignable_v<value_type>);
        static_assert(std::is_nothrow_destructible_v<value_type>);

        queue() = delete;

        size_type size() const noexcept { return this->tail.load(std::memory_order_relaxed); }
        size_type capacity() const noexcept { return this->capacity_value; }

        struct alignas(details::queue_cell_align) cell
        {
            value_type value = value_type();
        };

        static std::unique_ptr<queue> make(size_type capacity) {
            std::byte* const raw = reinterpret_cast<std::byte*>(
                ::operator new(sizeof(queue) + sizeof(cell) * capacity)
            );
            std::uninitialized_default_construct_n(reinterpret_cast<cell*>(raw + sizeof(queue)), capacity);
            // noexcept
            queue* const ptr = new(raw) queue(capacity);
            return std::unique_ptr<queue>(ptr);
        }

        void operator delete(queue* ptr, std::destroying_delete_t) noexcept {
            const size_t capacity = ptr->capacity();
            ptr->~queue();
            std::destroy_n(std::make_reverse_iterator(to_storage_ptr(ptr) + capacity), capacity);
            ::operator delete(ptr);
        }

        bool enqueue(value_type&& v) noexcept {
            auto _ = this->guard();
            if (this->size() >= this->capacity()) return false;
            const auto i = this->tail.fetch_add(1, std::memory_order_relaxed);
            if (i >= this->capacity()) {
                this->tail.fetch_sub(1, std::memory_order_release);
                return false;
            }
            this->storage()[i] = std::move(v);
            return true;
        }

        void wait_for_exclusive() noexcept {
            auto _ = this->guard();
            while (this->shared_counter.load(std::memory_order_acquire) > 1) {
                std::this_thread::yield();
            }
        }

        std::span<cell> exclusive_values() noexcept {
            this->wait_for_exclusive();
            return this->storage().subspan(0, this->size());
        }

    private:
        explicit queue(size_type capacity) noexcept : capacity_value(capacity) {}
        ~queue() = default;

        static cell* to_storage_ptr(queue* self) noexcept {
            return std::launder(reinterpret_cast<cell*>(
                reinterpret_cast<std::byte*>(self) + sizeof(queue)
            ));
        }

        std::span<cell> storage() noexcept {
            return std::span<cell>(to_storage_ptr(this), this->capacity_value);
        }

        auto guard() noexcept {
            this->shared_counter.fetch_add(1, std::memory_order_relaxed);
            return details::defer([this] { this->shared_counter.fetch_sub(1, std::memory_order_release); });
        }

        std::atomic<size_type> tail = 0;
        std::atomic<size_type> shared_counter = 0;
        const size_type capacity_value;
    };

} // namespace mylib

#endif // MYLIB_THREAD_WORKER_H
