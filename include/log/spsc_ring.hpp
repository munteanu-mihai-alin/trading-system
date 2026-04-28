#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace hft::log {

// Header-only single-producer single-consumer ring buffer.
//
// One ring per producer thread; the logger background thread is the sole
// consumer.  The implementation reserves one slot to disambiguate full from
// empty, so usable capacity is Capacity - 1.
//
// T must be trivially copyable; the producer never blocks and never allocates.
template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert(Capacity >= 2, "Capacity must be >= 2");
  static_assert(std::is_trivially_copyable_v<T>,
                "T must be trivially copyable");

 public:
  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  bool try_push(const T& value) noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    const auto next = increment(head);
    if (next == tail_.load(std::memory_order_acquire)) {
      return false;
    }
    buffer_[head] = value;
    head_.store(next, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) {
      return false;
    }
    out = buffer_[tail];
    tail_.store(increment(tail), std::memory_order_release);
    return true;
  }

  bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
  }

  static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

 private:
  static constexpr std::size_t increment(std::size_t i) noexcept {
    return (i + 1) % Capacity;
  }

  alignas(64) std::array<T, Capacity> buffer_{};
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace hft::log
