#include "lob/engine.hpp"
#include "lob/runner.hpp"
#include <gtest/gtest.h>
#include <stdexcept>

TEST(Spsc, FullEmptyAndWrapAround) {
    lob::SpscQueue<std::uint64_t, 2> queue;
    std::uint64_t value{};
    EXPECT_FALSE(queue.try_pop(value));
    EXPECT_TRUE(queue.try_push(1)); EXPECT_TRUE(queue.try_push(2)); EXPECT_FALSE(queue.try_push(3));
    EXPECT_EQ(queue.producer_size(), 2U);
    EXPECT_TRUE(queue.try_pop(value)); EXPECT_EQ(value, 1U);
    EXPECT_TRUE(queue.try_push(3));
    EXPECT_TRUE(queue.try_pop(value)); EXPECT_EQ(value, 2U);
    EXPECT_TRUE(queue.try_pop(value)); EXPECT_EQ(value, 3U);
    EXPECT_FALSE(queue.try_pop(value));
}
TEST(Spsc, ConcurrentOrderAndShutdownDrainWithTinyQueue) {
    using namespace lob;
    std::vector<Command> commands;
    for (OrderId id = 1; id <= 100000; ++id) commands.push_back(Command::cancel(id, 1));
    OrderId next = 1;
    const auto stats = run_queued<2>(commands, [&](const auto& c) { ASSERT_EQ(c.id, next++); });
    EXPECT_EQ(stats.processed, commands.size()); EXPECT_EQ(next, 100001U);
    EXPECT_LE(stats.max_occupancy, 2U);
}
TEST(Spsc, WorkerExceptionsPropagateWithoutDeadlock) {
    std::vector<lob::Command> commands(10000);
    EXPECT_THROW(lob::run_queued<1>(commands, [](auto) { throw std::runtime_error("consumer failure"); }), std::runtime_error);
}
TEST(Spsc, EmptyInputShutsDown) {
    EXPECT_EQ(lob::run_queued<1>(std::vector<lob::Command>{}, [](auto) {}).processed, 0U);
}
TEST(Spsc, DirectAndQueuedEngineResultsAreIdentical) {
    using namespace lob;
    std::vector<Command> commands;
    for (OrderId id = 1; id <= 10000; ++id) {
        commands.push_back(Command::add(id, 1, id % 2 ? Side::Buy : Side::Sell, 100, 7));
        if (id % 5 == 0) commands.push_back(Command::modify(id, 1, 101, 4));
    }
    Engine direct, queued;
    std::vector<Event> expected, actual;
    for (auto c : commands) {
        const auto& e = direct.process(c); expected.insert(expected.end(), e.begin(), e.end());
    }
    auto stats = run_queued<8>(commands, [&](auto c) {
        const auto& e = queued.process(c); actual.insert(actual.end(), e.begin(), e.end());
    });
    EXPECT_EQ(stats.processed, commands.size()); EXPECT_EQ(actual, expected);
    EXPECT_EQ(queued.snapshot(), direct.snapshot()); EXPECT_EQ(queued.checksum(), direct.checksum());
}
