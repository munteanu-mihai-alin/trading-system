#pragma once
#include <atomic>

template<typename T, int N>
class SPSCQueue {
public:
    bool push(const T& v) {
        auto h = head.load(std::memory_order_relaxed);
        auto next = (h + 1) % N;

        if (next == tail.load(std::memory_order_acquire))
            return false;

        buffer[h] = v;
        head.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& v) {
        auto t = tail.load(std::memory_order_relaxed);

        if (t == head.load(std::memory_order_acquire))
            return false;

        v = buffer[t];
        tail.store((t + 1) % N, std::memory_order_release);
        return true;
    }

private:
    T buffer[N];
    std::atomic<int> head{0}, tail{0};
};
