#pragma once
#include <atomic>

namespace hft {

template <typename T, int N>
class SPSCQueue {
    T buffer_[N];
    std::atomic<int> head_{0};
    std::atomic<int> tail_{0};

public:
    bool push(const T& v) {
        const auto h = head_.load(std::memory_order_relaxed);
        const auto next = (h + 1) % N;
        if (next == tail_.load(std::memory_order_acquire)) return false;
        buffer_[h] = v;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& v) {
        const auto t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        v = buffer_[t];
        tail_.store((t + 1) % N, std::memory_order_release);
        return true;
    }
};

}  // namespace hft
