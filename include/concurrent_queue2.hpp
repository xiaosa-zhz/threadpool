#ifdef MYLIB_CONCURRENT_QUEUE_DEPRECATED_H

#ifndef MYLIB_CONCURRENT_QUEUE_H
#define MYLIB_CONCURRENT_QUEUE_H -1

// Deprecated header file for concurrent queue implementation

#include <concepts>
#include <cstddef>
#include <cstdint>

#include <iterator>
#include <ranges>
#include <functional>
#include <array>
#include <bit>
#include <new>
#include <atomic>
#include <thread>
#include <limits>
#include <memory>

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
            std::atomic_size_t tail = 0;
        };

        template<typename T>
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
            std::size_t size() const noexcept {
                return std::min(this->tail.load(std::memory_order_relaxed), capacity());
            }

            bool full() const noexcept { return this->tail.load(std::memory_order_relaxed) >= capacity(); }

            bool enqueue(value_type&& v) noexcept {
                if (this->exclusive.test(std::memory_order_relaxed)) {
                    return false;
                }
                auto _ = this->guard();
                if (full()) return false;
                const auto i = this->tail.fetch_add(1, std::memory_order_relaxed);
                if (i >= capacity()) return false;
                storage()[i] = std::move(v);
                return true;
            }

            std::span<cell_type> wait_for_exclusive_values() noexcept {
                this->exclusive.test_and_set(std::memory_order_relaxed);
                while (this->shared_counter.load(std::memory_order_acquire) > 0) {
                    std::this_thread::yield();
                }
                this->exclusive.clear(std::memory_order_relaxed);
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
                ::operator delete(ptr);
            }

        private:
            explicit queue_buffer(std::size_t capacity) noexcept : queue_head(capacity) {}

            static cell_type* to_storage_ptr(queue_buffer* ptr) noexcept {
                return std::launder(reinterpret_cast<cell_type*>(
                    reinterpret_cast<std::byte*>(ptr) + sizeof(cell_type)
                ));
            }

            auto guard() noexcept {
                this->shared_counter.fetch_add(1, std::memory_order_relaxed);
                return details::defer([this] { this->shared_counter.fetch_sub(1, std::memory_order_release); });
            }

            std::span<cell_type> storage() noexcept {
                return std::span<cell_type>(to_storage_ptr(this), capacity());
            }
        };

        template<typename T, std::size_t TAG_BIT_LENGTH = 3>
        struct tagged_pointer
        {
            constexpr static std::size_t alignment_requirement = (1ull << TAG_BIT_LENGTH);
            static_assert(alignof(T) >= alignment_requirement);
            constexpr static std::uintptr_t tag_mask = alignment_requirement - 1;
            constexpr static std::uintptr_t ptr_mask = ~tag_mask;
            static_assert(tag_mask + ptr_mask == ~std::uintptr_t(0));

            static tagged_pointer from_pointer(T* ptr) noexcept {
                return tagged_pointer(reinterpret_cast<std::uintptr_t>(ptr));
            }

            T* pointer() const noexcept { return reinterpret_cast<T*>(ptrv & ptr_mask); }
            std::uintptr_t tag() const noexcept { return ptrv & tag_mask; }

            std::uintptr_t ptrv;
        };

    } // namespace mylib::details

    template<typename T>
    class queue
    {
    public:
        using value_type = T;
        using queue_unit_type = details::queue_buffer<value_type>;
        using queue_unit_handle_type = std::unique_ptr<queue_unit_type>;
    private:
        using cell_type = details::queue_cell<value_type>;
    public:
        class [[nodiscard("Contents of queue should be consumed.")]] values_view
            : public std::ranges::view_interface<values_view>
        {
        public:
            struct iterator
            {
                using iterator_concept = std::random_access_iterator_tag;
                using value_type = queue::value_type;
                using difference_type = std::ptrdiff_t;

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
            using original_view = std::span<details::queue_cell<value_type>>;

            friend queue;
            explicit values_view(original_view o) noexcept : original(o) {}

            original_view original = original_view();
        };

        static_assert(std::ranges::random_access_range<values_view>);

        queue() : queue(128) {}

        explicit queue(std::size_t capacity) :
            enqueue_handle(queue_unit_handle_type::make(capacity)),
            dequeue_handle(queue_unit_handle_type::make(capacity)),
            enqueue_ptr(enqueue_handle.get())
        {}

        ~queue() { (void) stop(); }

        queue(const queue&) = delete;
        queue& operator=(const queue&) = delete;

        void start() noexcept {
            enqueue_ptr.store(enqueue_handle.get(), std::memory_order_relaxed);
            stealing.clear(std::memory_order_release);
        }

        values_view stop() noexcept {
            while (stealing.test_and_set(std::memory_order_acq_rel));
            return enqueue_ptr.exchange(nullptr, std::memory_order_acquire)->wait_for_exclusive_values();
        }

        [[nodiscard]]
        bool enqueue(value_type&& v) noexcept {
            const std::uintptr_t raw = this->enqueue_ptr.load(std::memory_order_relaxed);
            if (queue_unit_type* const cur = to_pointer(raw)) {
                return cur->enqueue(std::move(v));
            } else {
                return false;
            }
        }

        values_view fetch_values() noexcept {
            switch_enqueue_handle(dequeue_handle);
            return wait_for_exclusive_values();
        }

        values_view steal(queue& other) noexcept {
            other.switch_enqueue_handle(this->dequeue_handle);
            return wait_for_exclusive_values();
        }

    private:
        constexpr static std::uintptr_t tag_mask = 1;
        constexpr static std::uintptr_t ptr_mask = ~tag_mask;

        static queue_unit_type* to_pointer(std::uintptr_t raw) noexcept {
            return reinterpret_cast<queue_unit_type*>(raw & ptr_mask);
        }

        static std::uintptr_t from_pointer(queue_unit_type* ptr) noexcept {
            return reinterpret_cast<std::uintptr_t>(ptr);
        }

        void switch_enqueue_handle(queue_unit_handle_type& new_enqueue_handle) noexcept {
            std::uintptr_t raw = this->enqueue_ptr.load(std::memory_order_relaxed);
            do {
                if (raw & tag_mask) {
                    std::this_thread::yield();
                    raw = this->enqueue_ptr.load(std::memory_order_relaxed);
                    continue;
                }
            } while (!this->enqueue_ptr.compare_exchange_strong(
                raw, (raw | std::uintptr_t(1)), std::memory_order_acq_rel));
            std::ranges::swap(this->enqueue_handle, new_enqueue_handle);
            enqueue_ptr.store(from_pointer(this->enqueue_handle.get()), std::memory_order_release);
        }

        values_view wait_for_exclusive_values() noexcept {
            return values_view(dequeue_handle->wait_for_exclusive_values());
        }

        struct alignas(details::queue_align) {
            std::atomic<std::uintptr_t> enqueue_ptr;
            queue_unit_handle_type enqueue_handle;
        };
        queue_unit_handle_type dequeue_handle;
    };
    
} // namespace mylib

#endif // MYLIB_CONCURRENT_QUEUE_H

#endif // MYLIB_CONCURRENT_QUEUE_DEPRECATED_H
