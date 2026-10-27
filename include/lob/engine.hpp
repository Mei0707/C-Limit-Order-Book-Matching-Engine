#pragma once

#include "lob/types.hpp"
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace lob {
enum class AllocationMode { Baseline, Pooled };
struct EngineConfig {
    AllocationMode allocation{AllocationMode::Pooled};
    std::size_t expected_orders{4096};
    std::size_t event_reserve{64};
};
struct MemoryStats {
    std::uint64_t logical_allocations{};
    std::uint64_t upstream_allocations{};
    std::size_t upstream_bytes{};
    std::size_t peak_upstream_bytes{};
};

// Single-writer API. Returned events are valid until the next process() call.
// Query APIs must run on the owner thread or after it has stopped.
class Engine {
public:
    explicit Engine(std::vector<Instrument> instruments = {{1, 1}}, EngineConfig config = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    const std::vector<Event>& process(const Command& command);
    std::optional<Order> find(OrderId id) const;
    std::optional<DepthLevel> best_bid(InstrumentId instrument) const;
    std::optional<DepthLevel> best_ask(InstrumentId instrument) const;
    Depth depth(InstrumentId instrument, std::size_t levels = 0) const;
    Snapshot snapshot() const;
    std::uint64_t checksum() const;
    std::uint64_t sequence() const;
    std::size_t active_orders() const;
    MemoryStats memory_stats() const;
    // Empty means valid; diagnostic string describes the first broken invariant.
    std::string check_invariants() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lob
