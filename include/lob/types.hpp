#pragma once

#include <cstdint>
#include <vector>

namespace lob {
using OrderId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint64_t;
using InstrumentId = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };
enum class TimeInForce : std::uint8_t { GTC, IOC, FOK };
enum class CommandType : std::uint8_t { Add, Cancel, Modify };
enum class EventType : std::uint8_t { Accepted, Rejected, Amended, Trade, Canceled };
enum class Reason : std::uint8_t {
    None, InvalidInstrument, InvalidId, DuplicateId, UnknownOrder, InvalidSide,
    InvalidType, InvalidTimeInForce, InvalidQuantity, InvalidPrice,
    UnsupportedCombination, QuantityOverflow, UserRequested, IOCExpired,
    MarketExpired, FOKUnfilled, InvalidCommand, SequenceExhausted
};

struct Instrument {
    InstrumentId id;
    Price tick_size{1};
    bool operator==(const Instrument&) const = default;
};

// Fixed-size, trivially copyable commands can also be sent through SpscQueue.
struct Command {
    CommandType kind{CommandType::Add};
    OrderId id{};
    InstrumentId instrument{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    TimeInForce tif{TimeInForce::GTC};
    Price price{};
    Quantity quantity{};

    static Command add(OrderId id, InstrumentId instrument, Side side, Price price,
                       Quantity quantity, OrderType type = OrderType::Limit,
                       TimeInForce tif = TimeInForce::GTC) {
        return {CommandType::Add, id, instrument, side, type, tif, price, quantity};
    }
    static Command cancel(OrderId id, InstrumentId instrument) {
        Command c;
        c.kind = CommandType::Cancel; c.id = id; c.instrument = instrument;
        return c;
    }
    static Command modify(OrderId id, InstrumentId instrument, Price price, Quantity quantity) {
        auto c = cancel(id, instrument);
        c.kind = CommandType::Modify; c.price = price; c.quantity = quantity;
        return c;
    }
    bool operator==(const Command&) const = default;
};

struct Order {
    OrderId id;
    InstrumentId instrument;
    Side side;
    Price price;
    Quantity remaining_qty;
    std::uint64_t sequence;
    bool operator==(const Order&) const = default;
};

struct Event {
    EventType type{};
    std::uint64_t sequence{}; // Accepted-command sequence; zero on rejection.
    OrderId order_id{};      // Taker on Trade events.
    OrderId maker_id{};
    InstrumentId instrument{};
    Side side{};
    Price price{};           // Resting maker price on Trade events.
    Quantity quantity{};
    Quantity remaining{};   // Taker remaining on Trade; zero on Canceled.
    Quantity maker_remaining{};
    Reason reason{Reason::None};
    bool operator==(const Event&) const = default;
};

struct DepthLevel {
    Price price;
    Quantity quantity;
    std::uint64_t order_count;
    bool operator==(const DepthLevel&) const = default;
};
struct Depth {
    std::vector<DepthLevel> bids;
    std::vector<DepthLevel> asks;
    bool operator==(const Depth&) const = default;
};
struct Snapshot {
    std::uint64_t sequence{};
    // 128-bit volume represented as two portable words, no overflow at uint64 max.
    std::uint64_t traded_volume_low{};
    std::uint64_t traded_volume_high{};
    std::vector<Instrument> instruments;
    std::vector<Order> orders; // Instrument, side, price priority, then FIFO.
    bool operator==(const Snapshot&) const = default;
};
} // namespace lob
