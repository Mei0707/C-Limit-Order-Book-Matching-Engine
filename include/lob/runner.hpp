#pragma once

#include "lob/spsc_queue.hpp"
#include "lob/types.hpp"
#include <algorithm>
#include <atomic>
#include <exception>
#include <thread>
#include <vector>

namespace lob {
struct QueueStats {
    std::uint64_t processed{};
    std::uint64_t full_retries{};
    std::size_t max_occupancy{};
};

// Owns worker lifetime: producer retries full queues, then signals completion.
// Worker drains before joining. Consumer exceptions stop the producer and rethrow.
// The caller must not access the engine captured by consume until this returns.
template <std::size_t Capacity = 4096, class Consumer>
QueueStats run_queued(const std::vector<Command>& commands, Consumer consume) {
    SpscQueue<Command, Capacity> queue;
    std::atomic<bool> done{false}, failed{false};
    std::exception_ptr error;
    QueueStats stats;
    std::uint64_t processed = 0;
    std::thread worker([&] {
        try {
            Command c;
            for (;;) {
                if (queue.try_pop(c)) { consume(c); ++processed; }
                else if (done.load(std::memory_order_acquire)) {
                    // Recheck after acquiring the completion signal.
                    if (!queue.try_pop(c)) break;
                    consume(c); ++processed;
                } else std::this_thread::yield();
            }
        } catch (...) {
            error = std::current_exception();
            failed.store(true, std::memory_order_release);
        }
    });
    for (const auto& c : commands) {
        while (!failed.load(std::memory_order_acquire) && !queue.try_push(c)) {
            ++stats.full_retries;
            std::this_thread::yield();
        }
        if (failed.load(std::memory_order_acquire)) break;
        stats.max_occupancy = std::max(stats.max_occupancy, queue.producer_size());
    }
    done.store(true, std::memory_order_release);
    worker.join();
    if (error) std::rethrow_exception(error);
    stats.processed = processed;
    return stats;
}
} // namespace lob
