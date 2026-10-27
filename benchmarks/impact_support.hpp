#pragma once

#include "lob/engine.hpp"
#include <algorithm>
#include <random>
#include <stdexcept>

namespace impact {
using namespace lob;
inline constexpr std::uint64_t SampleStride = 127;
inline constexpr InstrumentId Instruments = 4;
inline constexpr std::size_t BaseLevelsPerSide = 16;

inline std::vector<Instrument> instruments() {
    std::vector<Instrument> result;
    for (InstrumentId i = 1; i <= Instruments; ++i) result.push_back({i, 1});
    return result;
}
inline Command initial_order(std::size_t index) {
    const bool buy = index % 2 == 0;
    const auto instrument = static_cast<InstrumentId>((index / 2) % Instruments + 1);
    const auto distance = static_cast<Price>((index / (2 * Instruments)) % BaseLevelsPerSide);
    return Command::add(index + 1, instrument, buy ? Side::Buy : Side::Sell,
                        buy ? 999000 - distance : 1001000 + distance, 100);
}
inline void populate(Engine& engine, std::size_t active) {
    for (std::size_t i = 0; i < active; ++i) {
        const auto& events = engine.process(initial_order(i));
        if (events.size() != 1 || events.front().type != EventType::Accepted)
            throw std::runtime_error("Initial book population failed");
    }
}
// Every eight-command group restores initial quantities and live IDs. Random
// targets span the entire book; sequence/FIFO priority intentionally evolves.
// Two inside-spread trades execute 30 + 80 units without consuming base orders.
inline std::vector<Command> mixed_batch(std::size_t active, std::size_t count, std::uint64_t seed) {
    if (!active || !count || count % 8) throw std::invalid_argument("Use positive active count and a batch divisible by eight");
    std::mt19937_64 random(seed);
    std::vector<Command> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count / 8; ++i) {
        auto target = initial_order(static_cast<std::size_t>(random() % active));
        result.push_back(Command::cancel(target.id, target.instrument));
        result.push_back(target);
        result.push_back(Command::modify(target.id, target.instrument, target.price, 80));
        result.push_back(Command::modify(target.id, target.instrument, target.price, 100));
        auto side = random() % 2 ? Side::Buy : Side::Sell;
        auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
        auto instrument = static_cast<InstrumentId>(1 + random() % Instruments);
        const Price price = side == Side::Buy ? 999900 : 1000100;
        result.push_back(Command::add(active + 1, instrument, side, price, 100));
        result.push_back(Command::add(active + 2, instrument, opposite, price, 30,
                                      OrderType::Limit, TimeInForce::IOC));
        result.push_back(Command::modify(active + 1, instrument, price, 80));
        result.push_back(Command::add(active + 2, instrument, opposite, 0, 80,
                                      OrderType::Market, TimeInForce::FOK));
    }
    return result;
}
inline std::uint64_t quantile(std::vector<std::uint64_t> values, std::size_t percent) {
    if (values.empty()) return 0;
    if (percent == 0 || percent > 100) throw std::invalid_argument("Percentile must be 1..100");
    const auto rank = (values.size() / 100) * percent + ((values.size() % 100) * percent + 99) / 100;
    auto it = values.begin() + static_cast<std::ptrdiff_t>(rank - 1);
    std::nth_element(values.begin(), it, values.end());
    return *it;
}
inline void verify(const Engine& engine, std::size_t active, std::uint64_t measured,
                   std::uint64_t warm, std::uint64_t trades, std::uint64_t rejected) {
    if (rejected || engine.active_orders() != active || engine.sequence() != active + warm + measured ||
        trades != (measured / 8) * 2) throw std::runtime_error("Workload accounting mismatch");
    const auto snapshot = engine.snapshot();
    if (snapshot.traded_volume_high || snapshot.traded_volume_low != ((warm + measured) / 8) * 110)
        throw std::runtime_error("Traded volume mismatch");
    for (const auto& o : snapshot.orders) {
        if (o.id == 0 || o.id > active) throw std::runtime_error("Unexpected live ID");
        const auto initial = initial_order(static_cast<std::size_t>(o.id - 1));
        if (o.instrument != initial.instrument || o.side != initial.side ||
            o.price != initial.price || o.remaining_qty != initial.quantity)
            throw std::runtime_error("Base order unexpectedly changed");
    }
    auto error = engine.check_invariants();
    if (!error.empty()) throw std::runtime_error(error);
}
} // namespace impact
