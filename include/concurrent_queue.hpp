#ifndef MYLIB_CONCURRENT_QUEUE_H
#define MYLIB_CONCURRENT_QUEUE_H 1

/*
    * Header file for concurrent queue implementation
    * 
    * This file provides a thread-safe queue that allows multiple producers and ONE consumer.
    * It is designed to have a dequeue API different from the OG queue API
    * which fetches all values enqueued since the last dequeue call at once.
*/

#include <concepts>
#include <cstddef>

#include <iterator>
#include <ranges>
#include <array>
#include <new>
#include <atomic>
#include <thread>
#include <memory>
#include <cassert>

namespace mylib {

    namespace details {
    
        template<typename F>
        struct defer {
            ~defer() { this->f(); }
            F f;
        };
    
        inline constexpr std::size_t queue_align = std::hardware_destructive_interference_size;

        struct alignas(queue_align) queue_head
        {
            const std::size_t capacity;
            std::size_t size = 0;
            std::atomic_size_t leaving_counter = 0;
        };

        template<typename T>
            requires std::default_initializable<T> && std::destructible<T> && (std::is_nothrow_swappable_v<T>)
        struct alignas(queue_align) queue_cell
        {
            T value;
        };

        template<typename T>
        class queue_buffer : private queue_head
        {
        public:
            using value_type = T;
            using cell_type = queue_cell<value_type>;
            constexpr static std::size_t final_alignment = alignof(cell_type);

            static_assert(std::is_default_constructible_v<value_type>);
            static_assert(std::is_nothrow_move_assignable_v<value_type>);
            static_assert(std::is_nothrow_destructible_v<value_type>);
            static_assert(sizeof(queue_head) <= sizeof(cell_type));

            std::size_t capacity() const noexcept { return this->queue_head::capacity; }
            std::size_t size() const noexcept { return this->queue_head::size; }

            bool full() const noexcept { return this->leaving_counter.load(std::memory_order_relaxed) >= capacity(); }

            bool enqueue(std::size_t queue_number, value_type&& v) noexcept {
                defer _([this] { this->leaving_counter.fetch_add(1, std::memory_order_release); });
                if (queue_number >= capacity()) {
                    return false;
                }
                std::ranges::swap(v, storage()[queue_number].value);
                return true;
            }

            std::span<cell_type> wait_for_exclusive_values(std::size_t total_candidates) noexcept {
                while (this->leaving_counter.load(std::memory_order_acquire) < total_candidates) {
                    std::this_thread::yield(); // TODO: maybe do something else while waiting?
                }
                this->leaving_counter.store(0, std::memory_order_relaxed);
                this->queue_head::size = std::min(total_candidates, capacity());
                return storage().subspan(0, size());
            }

            static std::unique_ptr<queue_buffer> make(std::size_t capacity) {
                // value_type may grant even bigger alignment to cell
                // align the whole storage (including queue head) to the overall biggest alignment
                std::byte* const raw = reinterpret_cast<std::byte*>(::operator new(
                    sizeof(cell_type) * (capacity + 1),
                    std::align_val_t(final_alignment)
                ));
                std::uninitialized_default_construct_n(reinterpret_cast<cell_type*>(raw + sizeof(cell_type)), capacity);
                // noexcept
                queue_buffer* const ptr = new(raw) queue_buffer(capacity);
                return std::unique_ptr<queue_buffer>(ptr);
            }

            void operator delete(queue_buffer* ptr, std::destroying_delete_t) noexcept {
                const size_t capacity = ptr->capacity();
                ptr->~queue_buffer();
                std::destroy_n(std::make_reverse_iterator(to_storage_ptr(ptr) + capacity), capacity);
                ::operator delete(ptr, std::align_val_t(final_alignment));
            }

        private:
            explicit queue_buffer(std::size_t capacity) noexcept : queue_head(capacity) {}
            queue_buffer(const queue_buffer&) = delete;
            queue_buffer& operator=(const queue_buffer&) = delete;
            queue_buffer(queue_buffer&&) = delete;
            queue_buffer& operator=(queue_buffer&&) = delete;

            static cell_type* to_storage_ptr(queue_buffer* ptr) noexcept {
                return std::launder(reinterpret_cast<cell_type*>(
                    reinterpret_cast<std::byte*>(ptr) + sizeof(cell_type)
                ));
            }

            std::span<cell_type> storage() noexcept {
                return std::span<cell_type>(to_storage_ptr(this), capacity());
            }
        };
        
    } // namespace details

    template<typename T>
    class alignas(std::hardware_constructive_interference_size) concurrent_queue
    {
    public:
        using value_type = T;
        using queue_unit_type = details::queue_buffer<value_type>;
        using queue_unit_handle_type = std::unique_ptr<queue_unit_type>;
        constexpr static std::size_t top_bit_mask = ~((~0uz) >> 1);
    private:
        using cell_type = details::queue_cell<value_type>;
    public:
        class [[nodiscard("Contents of queue should be consumed.")]] values_view
            : public std::ranges::view_interface<values_view>
        {
        public:
            class iterator
            {
            public:
                using iterator_category = std::random_access_iterator_tag;
                using difference_type = std::ptrdiff_t;
                using value_type = concurrent_queue::value_type;
                using reference = value_type&;

                iterator() = default;
                iterator(const iterator&) = default;
                iterator& operator=(const iterator&) = default;

                friend auto operator<=>(const iterator&, const iterator&) = default;

                value_type& operator*() const noexcept { return original->value; }
                value_type& operator[](difference_type i) const noexcept { return (original + i)->value; }

                iterator& operator++() noexcept { ++original; return *this; }
                iterator operator++(int) noexcept { iterator cached = *this; ++*this; return cached; }
                iterator& operator--() noexcept { --original; return *this; }
                iterator operator--(int) noexcept { iterator cached = *this; --*this; return cached; }

                friend difference_type operator-(const iterator& lhs, const iterator& rhs) noexcept { return lhs.original - rhs.original; }

                iterator& operator+=(difference_type diff) noexcept { original += diff; return *this; }
                friend iterator operator+(const iterator& iter, difference_type diff) noexcept { return iterator(iter.original + diff); }
                friend iterator operator+(difference_type diff, const iterator& iter) noexcept { return iterator(iter.original + diff); }

                iterator& operator-=(difference_type diff) noexcept { original -= diff; return *this; }
                friend iterator operator-(const iterator& iter, difference_type diff) noexcept { return iterator(iter.original - diff); }
                friend iterator operator-(difference_type diff, const iterator& iter) noexcept { return iterator(iter.original - diff); }

            private:
                friend values_view;
                explicit iterator(cell_type* original) noexcept : original(original) {}

                cell_type* original = nullptr;
            };

            values_view() = default;
            values_view(const values_view&) = default;
            values_view& operator=(const values_view&) = default;

            iterator begin() const noexcept { return iterator(original.data()); }
            iterator end() const noexcept { return iterator(original.data() + original.size()); }
            
        private:
            using original_view = std::span<typename queue_unit_type::cell_type>;
            friend concurrent_queue;
            explicit values_view(original_view o) noexcept : original(o) {}

            original_view original = original_view();
        };

        static_assert(std::ranges::random_access_range<values_view>);

        concurrent_queue() = delete;
        concurrent_queue(const concurrent_queue&) = delete;
        concurrent_queue& operator=(const concurrent_queue&) = delete;
        concurrent_queue(concurrent_queue&&) = delete;
        concurrent_queue& operator=(concurrent_queue&&) = delete;

        explicit concurrent_queue(std::size_t capacity)
            : queue_handles{
                queue_unit_type::make(capacity),
                queue_unit_type::make(capacity),
                queue_unit_type::make(capacity)
            }
        {}

        bool enqueue(value_type&& v) noexcept {
            if (this->full_flag.load(std::memory_order_relaxed)) {
                return false;
            }
            const auto queue_token = this->entering_counter.fetch_add(1, std::memory_order_relaxed);
            const auto queue_index = queue_token & top_bit_mask;
            const auto queue_number = queue_token & ~top_bit_mask;
            const bool result = this->queue_handles[queue_index ? 1 : 0]->enqueue(queue_number, std::move(v));
            if (!result) {
                this->full_flag.store(true, std::memory_order_relaxed);
            }
            return result;
        }

        values_view wait_for_exclusive_values() noexcept {
            while (stealing_lock.test_and_set(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            details::defer _([this] { this->stealing_lock.clear(std::memory_order_release); });

            auto [queue_handle, total_candidates] = this->fetch_current_handle();
            values_view result(queue_handle->wait_for_exclusive_values(total_candidates));
            std::ranges::swap(queue_handle, this->queue_handles[2]);
            return result;
        }

        values_view steal(concurrent_queue& other) noexcept {
            if (other.stealing_lock.test_and_set(std::memory_order_acquire)) {
                return values_view();
            }
            details::defer _([&other] { other.stealing_lock.clear(std::memory_order_release); });

            auto [other_handle, total_candidates] = other.fetch_current_handle();
            values_view result(other_handle->wait_for_exclusive_values(total_candidates));
            std::ranges::swap(other_handle, this->queue_handles[2]);
            return result;
        }

    private:
        struct fetch_result {
            queue_unit_handle_type& handle;
            std::size_t total_candidates;
        };
        
        fetch_result fetch_current_handle() noexcept {
            const auto curr = this->entering_counter.load(std::memory_order_relaxed) & top_bit_mask;
            const auto next = curr ^ top_bit_mask;
            const auto final_result = this->entering_counter.exchange(next, std::memory_order_relaxed);
            this->full_flag.store(false, std::memory_order_relaxed);
            return { this->queue_handles[curr ? 1 : 0], final_result & ~top_bit_mask };
        }

        std::array<queue_unit_handle_type, 3> queue_handles;
        std::atomic_size_t entering_counter = 0;
        std::atomic_flag stealing_lock = {};
        std::atomic_bool full_flag = false;
    };

    static_assert(sizeof(concurrent_queue<std::size_t>) <= std::hardware_constructive_interference_size);

} // namespace mylib

#endif // MYLIB_CONCURRENT_QUEUE_H
