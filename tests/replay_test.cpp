#include "lob/replay.hpp"
#include <gtest/gtest.h>
#include <sstream>

TEST(Replay, RoundTripProducesIdenticalJournalsAndSnapshots) {
    using namespace lob;
    ReplayInput input{{{2, 5}, {1, 1}}, {
        Command::add(1, 1, Side::Buy, 100, 10), Command::modify(1, 1, 99, 11),
        Command::add(2, 1, Side::Sell, 0, 15, OrderType::Market, TimeInForce::IOC),
        Command::cancel(1, 1), Command::add(3, 2, Side::Buy, 100, 10)}};
    std::stringstream encoded;
    write_commands(encoded, input);
    auto decoded = read_commands(encoded);
    EXPECT_EQ(decoded.instruments, input.instruments); EXPECT_EQ(decoded.commands, input.commands);
    Engine first(input.instruments, EngineConfig{AllocationMode::Baseline});
    Engine second(decoded.instruments);
    std::ostringstream a, b;
    write_journal_header(a, input.instruments); write_journal_header(b, decoded.instruments);
    for (std::size_t i = 0; i < input.commands.size(); ++i) {
        const auto& ea = first.process(input.commands[i]);
        const auto& eb = second.process(decoded.commands[i]);
        write_journal_step(a, input.commands[i], ea, first.checksum());
        write_journal_step(b, decoded.commands[i], eb, second.checksum());
    }
    EXPECT_EQ(a.str(), b.str()); EXPECT_EQ(first.snapshot(), second.snapshot());
    a.str(""); b.str(""); write_snapshot(a, first); write_snapshot(b, second);
    EXPECT_EQ(a.str(), b.str());
}
TEST(Replay, StrictParserRejectsCorruptOrAmbiguousInput) {
    const std::vector<std::string> bad{
        "", "LOB_COMMANDS 2", "ADD 1 1 BUY LIMIT GTC 100 1", "LOB_COMMANDS 1\nBOGUS",
        "LOB_COMMANDS 1\nADD -1 1 BUY LIMIT GTC 100 1", "LOB_COMMANDS 1\nADD 1 1 BUY LIMIT GTC 1.5 1",
        "LOB_COMMANDS 1\nADD 1 1 BUY LIMIT GTC 100 18446744073709551616",
        "LOB_COMMANDS 1\nCANCEL 1 1 extra", "LOB_COMMANDS 1\nINSTRUMENT 1 0",
        "LOB_COMMANDS 1\nINSTRUMENT 1 1\nINSTRUMENT 1 1",
        "LOB_COMMANDS 1\nCANCEL 1 1\nINSTRUMENT 2 1"
    };
    for (const auto& text : bad) {
        std::istringstream in(text); EXPECT_THROW(lob::read_commands(in), std::invalid_argument) << text;
    }
}
TEST(Replay, CommentsAndMaximumIntegersAreSupported) {
    std::istringstream in("# hello\nLOB_COMMANDS 1\nADD 18446744073709551615 1 BUY LIMIT GTC 9223372036854775807 18446744073709551615 # comment\n");
    auto input = lob::read_commands(in);
    lob::Engine engine(input.instruments);
    EXPECT_EQ(engine.process(input.commands[0])[0].type, lob::EventType::Accepted);
    EXPECT_EQ(engine.check_invariants(), "");
}
TEST(Replay, ChecksumIncludesConfigurationSequenceAndOrderPriority) {
    lob::Engine first({{1, 1}}), second({{1, 5}});
    EXPECT_NE(first.checksum(), second.checksum());
    auto before = first.checksum();
    first.process(lob::Command::add(1, 1, lob::Side::Buy, 100, 1));
    first.process(lob::Command::cancel(1, 1));
    EXPECT_TRUE(first.snapshot().orders.empty()); EXPECT_NE(before, first.checksum());
}
