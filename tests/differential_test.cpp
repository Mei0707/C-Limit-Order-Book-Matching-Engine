#include "lob/engine.hpp"
#include "reference.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <random>

TEST(Differential, SeededRandomCommandsAgreeAfterEveryOperation) {
    using namespace lob;
    for (auto mode : {AllocationMode::Baseline, AllocationMode::Pooled}) {
        for (std::uint64_t seed = 1; seed <= 10; ++seed) {
            std::mt19937_64 rng(seed);
            Engine engine({{1, 1}, {2, 5}}, EngineConfig{mode});
            ReferenceMatcher reference({{1, 1}, {2, 5}});
            for (int step = 0; step < 2500; ++step) {
                SCOPED_TRACE(::testing::Message() << "seed=" << seed << " step=" << step << " mode=" << static_cast<int>(mode));
                const auto id = rng() % 500;
                const auto instrument = static_cast<InstrumentId>(rng() % 13 == 0 ? rng() % 4 : 1 + rng() % 2);
                const auto side = rng() % 2 ? Side::Buy : Side::Sell;
                const auto price = static_cast<Price>(95 + rng() % 15);
                const auto qty = rng() % 100;
                auto command = Command::add(id, instrument, side, price, qty);
                const auto choice = rng() % 100;
                if (choice < 20) command = Command::cancel(id, instrument);
                else if (choice < 40) command = Command::modify(id, instrument, price, qty);
                else {
                    command.tif = static_cast<TimeInForce>(rng() % 3);
                    if (rng() % 6 == 0) { command.type = OrderType::Market; command.price = 0; }
                    if (rng() % 80 == 0) command.quantity = std::numeric_limits<Quantity>::max();
                    if (rng() % 100 == 0) command.side = static_cast<Side>(9);
                    if (rng() % 100 == 0) command.type = static_cast<OrderType>(9);
                    if (rng() % 100 == 0) command.tif = static_cast<TimeInForce>(9);
                }
                auto actual = engine.process(command);
                auto expected = reference.process(command);
                ASSERT_EQ(actual, expected);
                ASSERT_EQ(engine.snapshot(), reference.snapshot());
                ASSERT_EQ(engine.check_invariants(), "");
                for (InstrumentId inst : {1U, 2U}) {
                    auto depth = reference.depth(inst);
                    ASSERT_EQ(engine.depth(inst), depth);
                    ASSERT_EQ(engine.best_bid(inst), depth.bids.empty() ? std::nullopt : std::optional<DepthLevel>{depth.bids.front()});
                    ASSERT_EQ(engine.best_ask(inst), depth.asks.empty() ? std::nullopt : std::optional<DepthLevel>{depth.asks.front()});
                }
                ASSERT_EQ(engine.find(id), reference.find(id));
            }
        }
    }
}
