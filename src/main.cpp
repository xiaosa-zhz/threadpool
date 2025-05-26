#include <print>

#include "concurrent_queue.hpp"

template<typename T>
struct flexible_array
{

};

struct noizy
{
    inline static std::size_t counter = 0;
    std::size_t self;
    noizy() noexcept : self(++counter) { std::println("C: {}", self); }
    ~noizy() { std::println("D: {}", self); }
};

int main() {
    auto q = mylib::queue<noizy>();
}
