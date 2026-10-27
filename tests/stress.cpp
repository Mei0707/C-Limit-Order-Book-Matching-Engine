#include "lob/engine.hpp"
#include "reference.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace {
using namespace lob;
std::uint64_t integer(const std::string& s) {
    std::uint64_t n{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || end != s.data() + s.size()) throw std::invalid_argument("Invalid integer: " + s);
    return n;
}
void feed(std::uint64_t& hash, std::uint64_t n) {
    for (int i = 0; i < 8; ++i) { hash ^= n & 255U; hash *= 1099511628211ULL; n >>= 8U; }
}
void digest(std::uint64_t& hash, const std::vector<Event>& events) {
    feed(hash, events.size());
    for (const auto& e : events) {
        feed(hash, static_cast<std::uint64_t>(e.type)); feed(hash, e.sequence);
        feed(hash, e.order_id); feed(hash, e.maker_id); feed(hash, e.instrument);
        feed(hash, static_cast<std::uint64_t>(e.side)); feed(hash, static_cast<std::uint64_t>(e.price));
        feed(hash, e.quantity); feed(hash, e.remaining); feed(hash, e.maker_remaining);
        feed(hash, static_cast<std::uint64_t>(e.reason));
    }
}
Command generate(std::mt19937_64& rng, const Snapshot& before, OrderId next_id) {
    const auto pick = rng() % 100;
    const bool has_live = !before.orders.empty();
    const auto existing = has_live ? before.orders[static_cast<std::size_t>(rng() % before.orders.size())]
                                   : Order{next_id + 1, 1, Side::Buy, 100, 1, 1};
    if (has_live && (before.orders.size() > 256 || (pick >= 45 && pick < 65)))
        return Command::cancel(existing.id, existing.instrument);
    if (has_live && pick >= 65 && pick < 90) {
        const auto tick = existing.instrument == 2 ? 5 : 1;
        auto price = rng() % 3 ? existing.price : static_cast<Price>(80 + rng() % 41);
        price -= price % tick;
        return Command::modify(existing.id, existing.instrument, price, rng() % 501);
    }
    const auto instrument = static_cast<InstrumentId>(1 + rng() % 4);
    const auto tick = instrument == 2 ? 5 : 1;
    auto price = static_cast<Price>(80 + rng() % 41);
    price -= price % tick;
    auto tif = rng() % 10 < 7 ? TimeInForce::GTC : rng() % 2 ? TimeInForce::IOC : TimeInForce::FOK;
    auto c = Command::add(next_id, instrument, rng() % 2 ? Side::Buy : Side::Sell, price, 1 + rng() % 500,
                          OrderType::Limit, tif);
    if (pick >= 90 || rng() % 10 == 0) {
        c.type = OrderType::Market; c.price = 0;
        c.tif = rng() % 3 ? TimeInForce::IOC : TimeInForce::FOK;
    }
    if (has_live && rng() % 25 == 0) c.id = existing.id;
    if (rng() % 100 == 0) c.quantity = 0;
    if (rng() % 100 == 0) c.instrument = 99;
    if (rng() % 100 == 0 && c.instrument == 2 && c.type == OrderType::Limit) ++c.price;
    return c;
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::uint64_t commands = 1000000, seeds = 20;
        std::string output = "stress.json";
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "lob_stress [--commands 1000000] [--seeds 20] [--output stress.json]\n";
                return 0;
            }
            if (++i == argc) throw std::invalid_argument("Missing value");
            if (arg == "--commands") commands = integer(argv[i]);
            else if (arg == "--seeds") seeds = integer(argv[i]);
            else if (arg == "--output") output = argv[i];
            else throw std::invalid_argument("Unknown option: " + arg);
        }
        if (!commands || commands > 10000000 || !seeds || seeds > 1000 || commands % seeds)
            throw std::invalid_argument("Use <=10M commands divisible by 1..1000 seeds");
        const std::vector<Instrument> specs{{1, 1}, {2, 5}, {3, 1}, {4, 1}};
        std::uint64_t accepted = 0, rejected = 0, trades = 0, max_active = 0, replayed = 0;
        std::ofstream out(output);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out << "{\n  \"schema\": 1,\n  \"commands\": " << commands << ",\n  \"seeds\": " << seeds
            << ",\n  \"seed_results\": [\n";
        for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
            std::mt19937_64 rng(seed);
            Engine baseline(specs, EngineConfig{AllocationMode::Baseline});
            Engine pooled(specs);
            ReferenceMatcher oracle(specs);
            std::vector<Command> stream;
            stream.reserve(static_cast<std::size_t>(commands / seeds));
            std::uint64_t event_hash = 14695981039346656037ULL;
            for (std::uint64_t step = 0; step < commands / seeds; ++step) {
                const auto expected_before = oracle.snapshot();
                const auto c = generate(rng, expected_before, seed * 100000000ULL + step + 1);
                stream.push_back(c);
                const auto expected_events = oracle.process(c);
                const auto expected_state = oracle.snapshot();
                const auto& base_events = baseline.process(c);
                const auto& pooled_events = pooled.process(c);
                const auto failure = [&](const std::string& why) {
                    throw std::runtime_error("seed=" + std::to_string(seed) + " step=" + std::to_string(step) + ": " + why);
                };
                if (base_events != expected_events || pooled_events != expected_events) failure("execution events differ");
                if (baseline.snapshot() != expected_state || pooled.snapshot() != expected_state) failure("order states/volume differ");
                if (!baseline.check_invariants().empty() || !pooled.check_invariants().empty()) failure("invariant violation");
                for (const auto spec : specs) {
                    const auto depth = oracle.depth(spec.id);
                    const auto bid = depth.bids.empty() ? std::nullopt : std::optional<DepthLevel>{depth.bids.front()};
                    const auto ask = depth.asks.empty() ? std::nullopt : std::optional<DepthLevel>{depth.asks.front()};
                    for (const auto* engine : {&baseline, &pooled})
                        if (engine->depth(spec.id) != depth || engine->best_bid(spec.id) != bid || engine->best_ask(spec.id) != ask)
                            failure("depth/quotes differ");
                }
                digest(event_hash, base_events);
                if (base_events.front().type == EventType::Rejected) ++rejected;
                else ++accepted;
                for (auto e : base_events) if (e.type == EventType::Trade) ++trades;
                max_active = std::max(max_active, static_cast<std::uint64_t>(expected_state.orders.size()));
            }
            Engine replay(specs);
            std::uint64_t replay_hash = 14695981039346656037ULL;
            for (const auto& c : stream) { digest(replay_hash, replay.process(c)); ++replayed; }
            if (event_hash != replay_hash || replay.snapshot() != pooled.snapshot() ||
                replay.checksum() != pooled.checksum() || baseline.checksum() != pooled.checksum())
                throw std::runtime_error("Replay failed for seed " + std::to_string(seed));
            if (seed > 1) out << ",\n";
            out << "    {\"seed\":" << seed << ",\"checksum\":" << pooled.checksum()
                << ",\"event_hash\":" << event_hash << ",\"replay_matches\":true}";
            std::cerr << "Differential + replay seed " << seed << '/' << seeds << " passed\n";
        }
        out << "\n  ],\n  \"accepted\": " << accepted << ",\n  \"rejected\": " << rejected
            << ",\n  \"trade_events\": " << trades << ",\n  \"max_active_orders\": " << max_active
            << ",\n  \"implementation_comparisons\": " << commands * 2
            << ",\n  \"state_comparisons\": " << commands * 2
            << ",\n  \"invariant_checks\": " << commands * 2
            << ",\n  \"replayed_commands\": " << replayed << ",\n  \"mismatches\": 0\n}\n";
        out.close();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
