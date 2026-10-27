#include "lob/engine.hpp"
#include <gtest/gtest.h>
#include <limits>

using namespace lob;
class EngineTest : public ::testing::TestWithParam<AllocationMode> {
protected:
    Engine engine{{{1, 1}, {2, 5}}, EngineConfig{GetParam()}};
    std::vector<Event> send(Command c) { return engine.process(c); }
    void TearDown() override { EXPECT_EQ(engine.check_invariants(), ""); }
};

TEST_P(EngineTest, ProposalMultilevelExample) {
    send(Command::add(1, 1, Side::Sell, 10000, 80));
    send(Command::add(2, 1, Side::Sell, 10050, 120));
    send(Command::add(3, 1, Side::Sell, 10100, 100));
    const auto e = send(Command::add(4, 1, Side::Buy, 10050, 150));
    ASSERT_EQ(e.size(), 3U);
    EXPECT_EQ(e[0].type, EventType::Accepted);
    EXPECT_EQ(e[1].maker_id, 1U); EXPECT_EQ(e[1].quantity, 80U); EXPECT_EQ(e[1].price, 10000);
    EXPECT_EQ(e[2].maker_id, 2U); EXPECT_EQ(e[2].quantity, 70U); EXPECT_EQ(e[2].price, 10050);
    EXPECT_EQ(engine.find(2)->remaining_qty, 50U);
    EXPECT_EQ(engine.best_ask(1)->quantity, 50U);
    EXPECT_FALSE(engine.find(4));
    EXPECT_EQ(engine.snapshot().traded_volume_low, 150U);
}
TEST_P(EngineTest, SellMatchesBestBidThenFifoAtMakerPrice) {
    send(Command::add(1, 1, Side::Buy, 100, 10));
    send(Command::add(2, 1, Side::Buy, 101, 20));
    send(Command::add(3, 1, Side::Buy, 101, 30));
    const auto e = send(Command::add(4, 1, Side::Sell, 99, 55));
    ASSERT_EQ(e.size(), 4U);
    EXPECT_EQ(e[1].maker_id, 2U); EXPECT_EQ(e[2].maker_id, 3U); EXPECT_EQ(e[3].maker_id, 1U);
    EXPECT_EQ(e[1].price, 101); EXPECT_EQ(e[3].price, 100);
    EXPECT_EQ(engine.best_bid(1)->quantity, 5U);
}
TEST_P(EngineTest, BuyMatchesLowestAskAndPreservesPartialMakerPriority) {
    send(Command::add(1, 1, Side::Sell, 101, 10));
    send(Command::add(2, 1, Side::Sell, 100, 10));
    send(Command::add(3, 1, Side::Sell, 100, 10));
    EXPECT_EQ(send(Command::add(4, 1, Side::Buy, 100, 5))[1].maker_id, 2U);
    EXPECT_EQ(send(Command::add(5, 1, Side::Buy, 100, 5))[1].maker_id, 2U);
    EXPECT_EQ(send(Command::add(6, 1, Side::Buy, 100, 5))[1].maker_id, 3U);
}
TEST_P(EngineTest, ResidualLimitRestsAndNonCrossingDoesNotTrade) {
    send(Command::add(1, 1, Side::Sell, 100, 10));
    send(Command::add(2, 1, Side::Buy, 99, 20));
    auto e = send(Command::add(3, 1, Side::Buy, 100, 15));
    ASSERT_EQ(e.size(), 2U);
    EXPECT_EQ(engine.find(3)->remaining_qty, 5U);
    EXPECT_EQ(engine.best_bid(1)->price, 100);
    EXPECT_FALSE(engine.best_ask(1));
}
TEST_P(EngineTest, EmptyMarketExpiresAndNeverRests) {
    auto e = send(Command::add(1, 1, Side::Buy, 0, 50, OrderType::Market, TimeInForce::IOC));
    ASSERT_EQ(e.size(), 2U);
    EXPECT_EQ(e[1].reason, Reason::MarketExpired); EXPECT_EQ(e[1].quantity, 50U);
    EXPECT_EQ(engine.active_orders(), 0U);
}
TEST_P(EngineTest, IocExecutesAvailableAndCancelsRemainder) {
    send(Command::add(1, 1, Side::Buy, 100, 10));
    auto e = send(Command::add(2, 1, Side::Sell, 100, 15, OrderType::Limit, TimeInForce::IOC));
    ASSERT_EQ(e.size(), 3U);
    EXPECT_EQ(e[1].quantity, 10U); EXPECT_EQ(e[2].quantity, 5U);
    EXPECT_EQ(e[2].reason, Reason::IOCExpired); EXPECT_EQ(engine.active_orders(), 0U);
}
TEST_P(EngineTest, FokFailureDoesNotMutateLiquidity) {
    send(Command::add(1, 1, Side::Sell, 100, 10));
    send(Command::add(2, 1, Side::Sell, 102, 20));
    auto before = engine.snapshot().orders;
    auto e = send(Command::add(3, 1, Side::Buy, 101, 11, OrderType::Limit, TimeInForce::FOK));
    ASSERT_EQ(e.size(), 2U);
    EXPECT_EQ(e[1].reason, Reason::FOKUnfilled); EXPECT_EQ(engine.snapshot().orders, before);
    EXPECT_EQ(engine.snapshot().traded_volume_low, 0U);
}
TEST_P(EngineTest, FokCanFillAcrossMultipleLevelsExactly) {
    send(Command::add(1, 1, Side::Sell, 100, 10));
    send(Command::add(2, 1, Side::Sell, 102, 20));
    auto e = send(Command::add(3, 1, Side::Buy, 102, 30, OrderType::Limit, TimeInForce::FOK));
    ASSERT_EQ(e.size(), 3U);
    EXPECT_EQ(e.back().type, EventType::Trade); EXPECT_EQ(engine.active_orders(), 0U);
}
TEST_P(EngineTest, MarketFokIgnoresPriceAndHasNoPartialFillsOnFailure) {
    send(Command::add(1, 1, Side::Buy, 1, 10));
    EXPECT_EQ(send(Command::add(2, 1, Side::Sell, 0, 11, OrderType::Market, TimeInForce::FOK)).back().reason,
              Reason::FOKUnfilled);
    auto e = send(Command::add(2, 1, Side::Sell, 0, 10, OrderType::Market, TimeInForce::FOK));
    EXPECT_EQ(e.back().price, 1); EXPECT_EQ(e.back().quantity, 10U);
}
TEST_P(EngineTest, CancelMiddleAndLastOrdersUpdatesDepthAndIndex) {
    for (OrderId id = 1; id <= 3; ++id) send(Command::add(id, 1, Side::Buy, 100, 10));
    EXPECT_EQ(send(Command::cancel(2, 1))[0].quantity, 10U);
    EXPECT_EQ(engine.best_bid(1)->quantity, 20U); EXPECT_EQ(engine.best_bid(1)->order_count, 2U);
    send(Command::cancel(1, 1)); send(Command::cancel(3, 1));
    EXPECT_FALSE(engine.best_bid(1)); EXPECT_EQ(engine.active_orders(), 0U);
    EXPECT_EQ(send(Command::cancel(3, 1))[0].reason, Reason::UnknownOrder);
}
TEST_P(EngineTest, QuantityDecreaseAndNoopPreserveTimePriority) {
    send(Command::add(1, 1, Side::Buy, 100, 10)); send(Command::add(2, 1, Side::Buy, 100, 10));
    send(Command::modify(1, 1, 100, 5)); send(Command::modify(1, 1, 100, 5));
    EXPECT_EQ(engine.find(1)->sequence, 1U);
    auto e = send(Command::add(3, 1, Side::Sell, 100, 6));
    ASSERT_EQ(e.size(), 3U); EXPECT_EQ(e[1].maker_id, 1U); EXPECT_EQ(e[2].maker_id, 2U);
}
TEST_P(EngineTest, QuantityIncreaseLosesTimePriority) {
    send(Command::add(1, 1, Side::Buy, 100, 10)); send(Command::add(2, 1, Side::Buy, 100, 10));
    send(Command::modify(1, 1, 100, 11));
    EXPECT_EQ(engine.find(1)->sequence, 3U);
    EXPECT_EQ(send(Command::add(3, 1, Side::Sell, 100, 1))[1].maker_id, 2U);
}
TEST_P(EngineTest, PriceChangeLosesPriorityAndCanExecuteImmediately) {
    send(Command::add(1, 1, Side::Buy, 99, 10)); send(Command::add(2, 1, Side::Sell, 101, 5));
    auto e = send(Command::modify(1, 1, 101, 7));
    ASSERT_EQ(e.size(), 2U); EXPECT_EQ(e[0].type, EventType::Amended);
    EXPECT_EQ(e[1].maker_id, 2U); EXPECT_EQ(e[1].price, 101);
    EXPECT_EQ(engine.find(1)->remaining_qty, 2U); EXPECT_EQ(engine.find(1)->sequence, 3U);
}
TEST_P(EngineTest, InvalidAmendmentLeavesOriginalUntouched) {
    send(Command::add(1, 2, Side::Buy, 100, 10)); auto before = engine.snapshot();
    EXPECT_EQ(send(Command::modify(1, 2, 101, 11))[0].reason, Reason::InvalidPrice);
    EXPECT_EQ(engine.snapshot(), before);
    EXPECT_EQ(send(Command::modify(1, 2, 100, 0))[0].reason, Reason::InvalidQuantity);
    EXPECT_EQ(engine.snapshot(), before);
}
TEST_P(EngineTest, InstrumentIsolationAndGlobalLiveIds) {
    send(Command::add(1, 1, Side::Buy, 100, 10));
    send(Command::add(2, 2, Side::Sell, 100, 10));
    EXPECT_EQ(engine.active_orders(), 2U);
    EXPECT_EQ(send(Command::add(1, 2, Side::Buy, 100, 10))[0].reason, Reason::DuplicateId);
    EXPECT_EQ(send(Command::cancel(1, 2))[0].reason, Reason::UnknownOrder);
    EXPECT_EQ(send(Command::modify(1, 2, 100, 5))[0].reason, Reason::UnknownOrder);
    send(Command::cancel(1, 1));
    EXPECT_EQ(send(Command::add(1, 2, Side::Buy, 100, 10))[0].type, EventType::Accepted);
}
TEST_P(EngineTest, InvalidInputsRejectWithoutSequenceOrStateChanges) {
    const std::vector<std::pair<Command, Reason>> bad{
        {Command::add(1, 0, Side::Buy, 100, 1), Reason::InvalidInstrument},
        {Command::add(0, 1, Side::Buy, 100, 1), Reason::InvalidId},
        {Command::add(1, 1, Side::Buy, 100, 0), Reason::InvalidQuantity},
        {Command::add(1, 1, Side::Buy, 0, 1), Reason::InvalidPrice},
        {Command::add(1, 1, Side::Buy, -100, 1), Reason::InvalidPrice},
        {Command::add(1, 2, Side::Buy, 101, 1), Reason::InvalidPrice},
        {Command::add(1, 1, Side::Buy, 0, 1, OrderType::Market), Reason::UnsupportedCombination},
        {Command::add(1, 1, Side::Buy, 1, 1, OrderType::Market, TimeInForce::IOC), Reason::InvalidPrice},
        {Command::add(1, 1, static_cast<Side>(3), 1, 1), Reason::InvalidSide},
        {Command::add(1, 1, Side::Buy, 1, 1, static_cast<OrderType>(9)), Reason::InvalidType},
        {Command::add(1, 1, Side::Buy, 1, 1, OrderType::Limit, static_cast<TimeInForce>(9)), Reason::InvalidTimeInForce}
    };
    auto before = engine.checksum();
    for (const auto& [c, reason] : bad) {
        auto e = send(c); ASSERT_EQ(e.size(), 1U); EXPECT_EQ(e[0].type, EventType::Rejected);
        EXPECT_EQ(e[0].reason, reason); EXPECT_EQ(e[0].sequence, 0U); EXPECT_EQ(engine.checksum(), before);
    }
    Command c; c.id = 1; c.instrument = 1; c.kind = static_cast<CommandType>(9);
    EXPECT_EQ(send(c)[0].reason, Reason::InvalidCommand);
}
TEST_P(EngineTest, AggregateOverflowRejectedAndLargeFokIsSafe) {
    auto max = std::numeric_limits<Quantity>::max();
    send(Command::add(1, 1, Side::Sell, 100, max));
    auto before = engine.snapshot();
    EXPECT_EQ(send(Command::add(2, 1, Side::Sell, 100, 1))[0].reason, Reason::QuantityOverflow);
    EXPECT_EQ(engine.snapshot(), before);
    send(Command::add(2, 1, Side::Sell, 101, max));
    EXPECT_EQ(send(Command::add(3, 1, Side::Buy, 101, max, OrderType::Limit, TimeInForce::FOK)).back().quantity, max);
    EXPECT_EQ(engine.snapshot().traded_volume_low, max);
    send(Command::add(4, 1, Side::Buy, 101, 2));
    EXPECT_EQ(engine.snapshot().traded_volume_low, 1U); EXPECT_EQ(engine.snapshot().traded_volume_high, 1U);
}
TEST_P(EngineTest, AmendmentAggregateOverflowExcludesOwnExistingQuantity) {
    auto max = std::numeric_limits<Quantity>::max();
    send(Command::add(1, 1, Side::Buy, 100, max - 5));
    send(Command::add(2, 1, Side::Buy, 100, 5));
    auto before = engine.snapshot();
    EXPECT_EQ(send(Command::modify(2, 1, 100, 6))[0].reason, Reason::QuantityOverflow);
    EXPECT_EQ(engine.snapshot(), before);
    EXPECT_EQ(send(Command::modify(1, 1, 100, max - 5))[0].type, EventType::Amended);
    EXPECT_EQ(engine.best_bid(1)->quantity, max);
}
TEST_P(EngineTest, DepthLimitsAndUnknownQueries) {
    send(Command::add(1, 1, Side::Buy, 100, 1)); send(Command::add(2, 1, Side::Buy, 99, 2));
    EXPECT_EQ(engine.depth(1, 1).bids.size(), 1U); EXPECT_EQ(engine.depth(1).bids.size(), 2U);
    EXPECT_THROW(engine.depth(99), std::out_of_range);
    EXPECT_THROW(engine.best_bid(99), std::out_of_range);
}
TEST_P(EngineTest, HighChurnAndLongLivedSamePriceOrders) {
    for (OrderId i = 1; i <= 20000; ++i) send(Command::add(i, 1, Side::Buy, 100, 2));
    for (OrderId i = 2; i <= 20000; i += 2) send(Command::cancel(i, 1));
    EXPECT_EQ(engine.best_bid(1)->quantity, 20000U);
    auto e = send(Command::add(20001, 1, Side::Sell, 0, 20000, OrderType::Market, TimeInForce::IOC));
    ASSERT_EQ(e.size(), 10001U);
    for (std::size_t i = 1; i < e.size(); ++i) EXPECT_EQ(e[i].maker_id, 2 * i - 1);
    EXPECT_EQ(engine.active_orders(), 0U);
}
INSTANTIATE_TEST_SUITE_P(Allocators, EngineTest,
    ::testing::Values(AllocationMode::Baseline, AllocationMode::Pooled),
    [](const ::testing::TestParamInfo<AllocationMode>& info) {
        return info.param == AllocationMode::Baseline ? "Baseline" : "Pooled";
    });
TEST(Configuration, RejectsInvalidInstrumentDefinitions) {
    EXPECT_THROW(Engine(std::vector<Instrument>{}), std::invalid_argument);
    EXPECT_THROW(Engine((std::vector<Instrument>{{1, 1}, {1, 5}})), std::invalid_argument);
    EXPECT_THROW(Engine((std::vector<Instrument>{{0, 1}})), std::invalid_argument);
    EXPECT_THROW(Engine((std::vector<Instrument>{{1, 0}})), std::invalid_argument);
}
