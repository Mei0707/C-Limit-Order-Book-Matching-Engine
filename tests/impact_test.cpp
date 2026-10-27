#include "impact_support.hpp"
#include <gtest/gtest.h>

TEST(ImpactWorkload, RepeatingRandomBatchPreservesOneThousandBaseOrdersAcrossFourInstruments) {
    using namespace impact;
    const auto batch = mixed_batch(1000, 8000, 42);
    Engine first(instruments(), EngineConfig{AllocationMode::Baseline});
    Engine second(instruments());
    populate(first, 1000); populate(second, 1000);
    std::uint64_t trades = 0;
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (const auto& c : batch) {
            const auto& a = first.process(c);
            const auto& b = second.process(c);
            ASSERT_EQ(a, b);
            ASSERT_NE(a.front().type, EventType::Rejected);
            for (const auto& e : a) if (e.type == EventType::Trade) ++trades;
        }
    }
    EXPECT_NO_THROW(verify(first, 1000, 24000, 0, trades, 0));
    EXPECT_NO_THROW(verify(second, 1000, 24000, 0, trades, 0));
    EXPECT_EQ(first.snapshot(), second.snapshot());
    EXPECT_EQ(first.checksum(), second.checksum());
    std::size_t levels = 0;
    for (auto spec : instruments()) {
        const auto depth = first.depth(spec.id);
        levels += depth.bids.size() + depth.asks.size();
    }
    EXPECT_EQ(levels, 128U);
}
TEST(ImpactWorkload, SeedIsReproducibleAndChangesSelectedTargets) {
    EXPECT_EQ(impact::mixed_batch(1000, 800, 7), impact::mixed_batch(1000, 800, 7));
    EXPECT_NE(impact::mixed_batch(1000, 800, 7), impact::mixed_batch(1000, 800, 8));
    EXPECT_THROW(impact::mixed_batch(0, 8, 1), std::invalid_argument);
    EXPECT_THROW(impact::mixed_batch(100, 7, 1), std::invalid_argument);
}
TEST(ImpactStatistics, NearestRankOnUnsortedAndEmptySamples) {
    EXPECT_EQ(impact::quantile({5, 1, 4, 2, 3}, 50), 3U);
    EXPECT_EQ(impact::quantile({5, 1, 4, 2, 3}, 99), 5U);
    EXPECT_EQ(impact::quantile({}, 99), 0U);
    EXPECT_THROW(impact::quantile({1}, 0), std::invalid_argument);
}
