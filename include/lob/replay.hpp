#pragma once

#include "lob/engine.hpp"
#include <iosfwd>
#include <string>

namespace lob {
struct ReplayInput {
    std::vector<Instrument> instruments;
    std::vector<Command> commands;
};
// Strict versioned text format. Throws with a line number on malformed input.
ReplayInput read_commands(std::istream& input);
void write_commands(std::ostream& output, const ReplayInput& input);
std::string format_event(const Event& event);
void write_snapshot(std::ostream& output, const Engine& engine);
void write_journal_header(std::ostream& output, const std::vector<Instrument>& instruments);
void write_journal_step(std::ostream& output, const Command& command,
                        const std::vector<Event>& events, std::uint64_t checksum);
std::string format_command(const Command& command);
} // namespace lob
