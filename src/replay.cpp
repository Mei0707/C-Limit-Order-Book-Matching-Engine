#include "lob/replay.hpp"

#include <charconv>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace lob {
namespace {
template <class T>
T number(std::string_view token) {
    T result{};
    auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), result);
    if (error != std::errc{} || end != token.data() + token.size())
        throw std::invalid_argument("Invalid integer: " + std::string(token));
    return result;
}
const char* side_name(Side s) { return s == Side::Buy ? "BUY" : "SELL"; }
const char* type_name(OrderType t) { return t == OrderType::Limit ? "LIMIT" : "MARKET"; }
const char* tif_name(TimeInForce t) {
    switch (t) { case TimeInForce::GTC: return "GTC"; case TimeInForce::IOC: return "IOC"; case TimeInForce::FOK: return "FOK"; }
    throw std::invalid_argument("Invalid time in force");
}
const char* event_name(EventType t) {
    switch (t) {
    case EventType::Accepted: return "ACCEPTED"; case EventType::Rejected: return "REJECTED";
    case EventType::Amended: return "AMENDED"; case EventType::Trade: return "TRADE";
    case EventType::Canceled: return "CANCELED";
    }
    throw std::invalid_argument("Invalid event type");
}
const char* reason_name(Reason r) {
    switch (r) {
    case Reason::None: return "NONE";
    case Reason::InvalidInstrument: return "INVALID_INSTRUMENT";
    case Reason::InvalidId: return "INVALID_ID";
    case Reason::DuplicateId: return "DUPLICATE_ID";
    case Reason::UnknownOrder: return "UNKNOWN_ORDER";
    case Reason::InvalidSide: return "INVALID_SIDE";
    case Reason::InvalidType: return "INVALID_TYPE";
    case Reason::InvalidTimeInForce: return "INVALID_TIF";
    case Reason::InvalidQuantity: return "INVALID_QUANTITY";
    case Reason::InvalidPrice: return "INVALID_PRICE";
    case Reason::UnsupportedCombination: return "UNSUPPORTED_COMBINATION";
    case Reason::QuantityOverflow: return "QUANTITY_OVERFLOW";
    case Reason::UserRequested: return "USER_REQUESTED";
    case Reason::IOCExpired: return "IOC_EXPIRED";
    case Reason::MarketExpired: return "MARKET_EXPIRED";
    case Reason::FOKUnfilled: return "FOK_UNFILLED";
    case Reason::InvalidCommand: return "INVALID_COMMAND";
    case Reason::SequenceExhausted: return "SEQUENCE_EXHAUSTED";
    }
    throw std::invalid_argument("Invalid reason");
}
} // namespace

ReplayInput read_commands(std::istream& input) {
    ReplayInput result;
    std::string line;
    std::size_t line_number = 0;
    bool header = false;
    while (std::getline(input, line)) {
        ++line_number;
        if (auto pos = line.find('#'); pos != std::string::npos) line.resize(pos);
        std::istringstream words(line);
        std::vector<std::string> t;
        for (std::string token; words >> token;) t.push_back(token);
        if (t.empty()) continue;
        try {
            if (!header) {
                if (t != std::vector<std::string>{"LOB_COMMANDS", "1"})
                    throw std::invalid_argument("Expected LOB_COMMANDS 1 header");
                header = true;
                continue;
            }
            if (t[0] == "INSTRUMENT" && t.size() == 3) {
                if (!result.commands.empty()) throw std::invalid_argument("Instruments must precede commands");
                Instrument spec{number<InstrumentId>(t[1]), number<Price>(t[2])};
                if (!spec.id || spec.tick_size <= 0) throw std::invalid_argument("Invalid instrument definition");
                for (const auto& old : result.instruments)
                    if (old.id == spec.id) throw std::invalid_argument("Duplicate instrument definition");
                result.instruments.push_back(spec);
            } else if (t[0] == "ADD" && t.size() == 8) {
                Side side;
                if (t[3] == "BUY") side = Side::Buy;
                else if (t[3] == "SELL") side = Side::Sell;
                else throw std::invalid_argument("Expected BUY or SELL");
                OrderType type;
                if (t[4] == "LIMIT") type = OrderType::Limit;
                else if (t[4] == "MARKET") type = OrderType::Market;
                else throw std::invalid_argument("Expected LIMIT or MARKET");
                TimeInForce tif;
                if (t[5] == "GTC") tif = TimeInForce::GTC;
                else if (t[5] == "IOC") tif = TimeInForce::IOC;
                else if (t[5] == "FOK") tif = TimeInForce::FOK;
                else throw std::invalid_argument("Expected GTC, IOC, or FOK");
                result.commands.push_back(Command::add(number<OrderId>(t[1]), number<InstrumentId>(t[2]),
                    side, number<Price>(t[6]), number<Quantity>(t[7]), type, tif));
            } else if (t[0] == "CANCEL" && t.size() == 3) {
                result.commands.push_back(Command::cancel(number<OrderId>(t[1]), number<InstrumentId>(t[2])));
            } else if (t[0] == "MODIFY" && t.size() == 5) {
                result.commands.push_back(Command::modify(number<OrderId>(t[1]), number<InstrumentId>(t[2]),
                    number<Price>(t[3]), number<Quantity>(t[4])));
            } else throw std::invalid_argument("Unknown command or incorrect number of fields");
        } catch (const std::exception& e) {
            throw std::invalid_argument("Line " + std::to_string(line_number) + ": " + e.what());
        }
    }
    if (input.bad()) throw std::runtime_error("Failed to read command stream");
    if (!header) throw std::invalid_argument("Missing LOB_COMMANDS 1 header");
    if (result.instruments.empty()) result.instruments.push_back({1, 1});
    return result;
}

std::string format_command(const Command& c) {
    std::ostringstream out;
    if (c.kind == CommandType::Add)
        out << "ADD " << c.id << ' ' << c.instrument << ' ' << side_name(c.side) << ' '
            << type_name(c.type) << ' ' << tif_name(c.tif) << ' ' << c.price << ' ' << c.quantity;
    else if (c.kind == CommandType::Cancel) out << "CANCEL " << c.id << ' ' << c.instrument;
    else if (c.kind == CommandType::Modify)
        out << "MODIFY " << c.id << ' ' << c.instrument << ' ' << c.price << ' ' << c.quantity;
    else throw std::invalid_argument("Invalid command kind");
    return out.str();
}
void write_commands(std::ostream& out, const ReplayInput& input) {
    out << "LOB_COMMANDS 1\n";
    for (const auto& s : input.instruments) out << "INSTRUMENT " << s.id << ' ' << s.tick_size << '\n';
    for (const auto& c : input.commands) out << format_command(c) << '\n';
}
std::string format_event(const Event& e) {
    std::ostringstream out;
    out << event_name(e.type) << ' ' << e.sequence << ' ' << e.order_id << ' ' << e.maker_id << ' '
        << e.instrument << ' ' << side_name(e.side) << ' ' << e.price << ' ' << e.quantity << ' '
        << e.remaining << ' ' << e.maker_remaining << ' ' << reason_name(e.reason);
    return out.str();
}
void write_journal_header(std::ostream& out, const std::vector<Instrument>& instruments) {
    out << "LOB_JOURNAL 1\n";
    for (const auto& s : instruments) out << "INSTRUMENT " << s.id << ' ' << s.tick_size << '\n';
}
void write_journal_step(std::ostream& out, const Command& c,
                        const std::vector<Event>& events, std::uint64_t checksum) {
    out << "COMMAND " << format_command(c) << '\n';
    for (const auto& e : events) out << "EVENT " << format_event(e) << '\n';
    out << "CHECKSUM " << checksum << '\n';
}
void write_snapshot(std::ostream& out, const Engine& engine) {
    const auto s = engine.snapshot();
    out << "{\n  \"version\": 1,\n  \"sequence\": " << s.sequence
        << ",\n  \"traded_volume_low\": " << s.traded_volume_low
        << ",\n  \"traded_volume_high\": " << s.traded_volume_high
        << ",\n  \"checksum\": " << engine.checksum() << ",\n  \"instruments\": [";
    bool first = true;
    for (auto spec : s.instruments) {
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << spec.id << ",\"tick_size\":" << spec.tick_size << '}';
    }
    out << "],\n  \"orders\": [";
    first = true;
    for (const auto& o : s.orders) {
        if (!first) out << ',';
        first = false;
        out << "\n    {\"id\":" << o.id << ",\"instrument\":" << o.instrument
            << ",\"side\":\"" << side_name(o.side) << "\",\"price\":" << o.price
            << ",\"remaining_qty\":" << o.remaining_qty << ",\"sequence\":" << o.sequence << '}';
    }
    out << "\n  ],\n  \"depth\": [";
    first = true;
    for (auto spec : s.instruments) {
        if (!first) out << ',';
        first = false;
        const auto d = engine.depth(spec.id);
        out << "\n    {\"instrument\":" << spec.id << ",\"bids\":[";
        const auto levels = [&](const auto& side) {
            bool first_level = true;
            for (auto level : side) {
                if (!first_level) out << ',';
                first_level = false;
                out << "{\"price\":" << level.price << ",\"quantity\":" << level.quantity
                    << ",\"order_count\":" << level.order_count << '}';
            }
        };
        levels(d.bids); out << "],\"asks\":["; levels(d.asks); out << "]}";
    }
    out << "\n  ]\n}\n";
}
} // namespace lob
