#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace lob {
// Exactly one producer and one consumer. Full queue rejects without overwriting.
// Capacity slots are usable; an additional internal slot distinguishes full/empty.
template <class T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity > 0);
    static_assert(std::is_trivially_copyable_v<T>);
public:
    bool try_push(const T& value) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = advance(head);
        if (next == tail_.load(std::memory_order_acquire)) return false;
        slots_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }
    bool try_pop(T& value) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        value = slots_[tail];
        tail_.store(advance(tail), std::memory_order_release);
        return true;
    }
    // Call from the producer: an upper estimate under concurrent consumption.
    std::size_t producer_size() const noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);
        return head >= tail ? head - tail : Slots - tail + head;
    }
    static constexpr std::size_t capacity() noexcept { return Capacity; }
private:
    static constexpr std::size_t Slots = Capacity + 1;
    static constexpr std::size_t advance(std::size_t n) noexcept { return (n + 1) % Slots; }
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::array<T, Slots> slots_{};
};
} // namespace lob
