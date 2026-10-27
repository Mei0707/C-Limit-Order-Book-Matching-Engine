#include "lob/engine.hpp"

#include <algorithm>
#include <limits>
#include <list>
#include <map>
#include "lob/memory.hpp"
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace lob {
namespace {
class CountingResource final : public memory::memory_resource {
public:
    explicit CountingResource(memory::memory_resource* upstream) : upstream_(upstream) {}
    std::uint64_t allocations{};
    std::size_t bytes{}, peak{};
private:
    void* do_allocate(std::size_t n, std::size_t alignment) override {
        auto* p = upstream_->allocate(n, alignment);
        ++allocations; bytes += n; peak = std::max(peak, bytes);
        return p;
    }
    void do_deallocate(void* p, std::size_t n, std::size_t alignment) override {
        upstream_->deallocate(p, n, alignment); bytes -= n;
    }
    bool do_is_equal(const memory::memory_resource& other) const noexcept override {
        return this == &other;
    }
    memory::memory_resource* upstream_;
};
struct Level {
    Price price;
    Quantity total{};
    memory::list<Order> orders;
    Level(Price p, memory::memory_resource* resource) : price(p), orders(resource) {}
};
struct Book {
    Instrument spec;
    memory::map<Price, Level, std::greater<Price>> bids;
    memory::map<Price, Level> asks;
    Book(Instrument s, memory::memory_resource* r) : spec(s), bids(r), asks(r) {}
};
struct Handle {
    Book* book;
    Level* level;
    memory::list<Order>::iterator order;
};
bool crosses(const Command& c, Price p) {
    return c.type == OrderType::Market || (c.side == Side::Buy ? p <= c.price : p >= c.price);
}
} // namespace

struct Engine::Impl {
    CountingResource upstream{memory::new_delete_resource()};
    memory::unsynchronized_pool_resource pool{&upstream};
    CountingResource logical;
    std::map<InstrumentId, std::unique_ptr<Book>> books;
    memory::unordered_map<OrderId, Handle> index;
    std::vector<Event> events;
    std::uint64_t sequence{}, volume_low{}, volume_high{};

    Impl(std::vector<Instrument> instruments, EngineConfig config)
        : logical(config.allocation == AllocationMode::Pooled
                      ? static_cast<memory::memory_resource*>(&pool) : &upstream), index(&logical) {
        if (instruments.empty()) throw std::invalid_argument("At least one instrument is required");
        for (auto spec : instruments) {
            if (spec.id == 0 || spec.tick_size <= 0 || books.contains(spec.id))
                throw std::invalid_argument("Instrument IDs must be unique and positive; tick size must be positive");
            books.emplace(spec.id, std::make_unique<Book>(spec, &logical));
        }
        index.reserve(config.expected_orders);
        events.reserve(config.event_reserve);
    }

    void emit(EventType type, const Command& c, Quantity qty, Quantity remaining,
              Reason reason = Reason::None) {
        events.push_back({type, type == EventType::Rejected ? 0 : sequence,
                          c.id, 0, c.instrument, c.side, c.price, qty, remaining, 0, reason});
    }
    Reason validate_add(const Command& c, const Book& b) const {
        if (c.side != Side::Buy && c.side != Side::Sell) return Reason::InvalidSide;
        if (c.type != OrderType::Limit && c.type != OrderType::Market) return Reason::InvalidType;
        if (c.tif != TimeInForce::GTC && c.tif != TimeInForce::IOC && c.tif != TimeInForce::FOK)
            return Reason::InvalidTimeInForce;
        if (!c.quantity) return Reason::InvalidQuantity;
        if (c.type == OrderType::Limit) {
            if (c.price <= 0 || c.price % b.spec.tick_size) return Reason::InvalidPrice;
        } else {
            if (c.price != 0) return Reason::InvalidPrice;
            if (c.tif == TimeInForce::GTC) return Reason::UnsupportedCombination;
        }
        return Reason::None;
    }
    bool aggregate_fits(const Command& c, const Book& b, Quantity exclude = 0) const {
        if (c.type != OrderType::Limit || c.tif != TimeInForce::GTC) return true;
        const auto fits = [&](const auto& levels) {
            auto it = levels.find(c.price);
            return it == levels.end() || c.quantity <= std::numeric_limits<Quantity>::max() - (it->second.total - exclude);
        };
        return c.side == Side::Buy ? fits(b.bids) : fits(b.asks);
    }
    template <class Levels>
    bool can_fill(const Command& c, const Levels& levels) const {
        auto needed = c.quantity;
        for (const auto& [price, level] : levels) {
            if (!crosses(c, price)) break;
            if (level.total >= needed) return true;
            needed -= level.total; // Subtraction avoids overflow while summing liquidity.
        }
        return false;
    }
    void unlink(Handle h) {
        auto id = h.order->id;
        auto side = h.order->side;
        auto price = h.order->price;
        h.level->total -= h.order->remaining_qty;
        h.level->orders.erase(h.order);
        index.erase(id);
        if (h.level->orders.empty()) {
            if (side == Side::Buy) h.book->bids.erase(price);
            else h.book->asks.erase(price);
        }
    }
    template <class Levels>
    void match(Command& incoming, Levels& opposite) {
        while (incoming.quantity && !opposite.empty()) {
            auto level_it = opposite.begin();
            auto& level = level_it->second;
            if (!crosses(incoming, level.price)) break;
            while (incoming.quantity && !level.orders.empty()) {
                auto& maker = level.orders.front();
                auto qty = std::min(incoming.quantity, maker.remaining_qty);
                incoming.quantity -= qty;
                maker.remaining_qty -= qty;
                level.total -= qty;
                const auto previous_volume = volume_low;
                volume_low += qty;
                if (volume_low < previous_volume) ++volume_high;
                events.push_back({EventType::Trade, sequence, incoming.id, maker.id,
                                  incoming.instrument, incoming.side, maker.price, qty,
                                  incoming.quantity, maker.remaining_qty, Reason::None});
                if (!maker.remaining_qty) {
                    index.erase(maker.id);
                    level.orders.pop_front();
                }
            }
            if (level.orders.empty()) opposite.erase(level_it);
        }
    }
    template <class Levels>
    void rest(const Command& c, Book& b, Levels& levels) {
        auto [it, created] = levels.try_emplace(c.price, c.price, &logical);
        (void)created;
        auto& level = it->second;
        level.orders.push_back({c.id, c.instrument, c.side, c.price, c.quantity, sequence});
        level.total += c.quantity;
        index.emplace(c.id, Handle{&b, &level, std::prev(level.orders.end())});
    }
    void execute(Command c, Book& b) {
        if (c.tif == TimeInForce::FOK) {
            const bool enough = c.side == Side::Buy ? can_fill(c, b.asks) : can_fill(c, b.bids);
            if (!enough) {
                emit(EventType::Canceled, c, c.quantity, 0, Reason::FOKUnfilled);
                return;
            }
        }
        if (c.side == Side::Buy) match(c, b.asks);
        else match(c, b.bids);
        if (!c.quantity) return;
        if (c.type == OrderType::Limit && c.tif == TimeInForce::GTC) {
            if (c.side == Side::Buy) rest(c, b, b.bids);
            else rest(c, b, b.asks);
        } else {
            emit(EventType::Canceled, c, c.quantity, 0,
                 c.type == OrderType::Market ? Reason::MarketExpired : Reason::IOCExpired);
        }
    }
    const std::vector<Event>& process(const Command& c) {
        events.clear();
        const auto reject = [&](Reason reason) -> const std::vector<Event>& {
            emit(EventType::Rejected, c, c.quantity, 0, reason); return events;
        };
        auto book_it = books.find(c.instrument);
        if (book_it == books.end()) return reject(Reason::InvalidInstrument);
        if (!c.id) return reject(Reason::InvalidId);
        if (sequence == std::numeric_limits<std::uint64_t>::max()) return reject(Reason::SequenceExhausted);
        auto& book = *book_it->second;
        if (c.kind == CommandType::Add) {
            if (index.contains(c.id)) return reject(Reason::DuplicateId);
            auto reason = validate_add(c, book);
            if (reason != Reason::None) return reject(reason);
            if (!aggregate_fits(c, book)) return reject(Reason::QuantityOverflow);
            ++sequence;
            emit(EventType::Accepted, c, c.quantity, c.quantity);
            execute(c, book);
        } else if (c.kind == CommandType::Cancel || c.kind == CommandType::Modify) {
            auto it = index.find(c.id);
            if (it == index.end() || it->second.book != &book) return reject(Reason::UnknownOrder);
            auto h = it->second;
            const auto old = *h.order;
            auto replacement = Command::add(c.id, c.instrument, old.side, old.price, old.remaining_qty);
            if (c.kind == CommandType::Cancel) {
                ++sequence;
                emit(EventType::Canceled, replacement, old.remaining_qty, 0, Reason::UserRequested);
                unlink(h);
            } else {
                replacement.price = c.price;
                replacement.quantity = c.quantity;
                auto reason = validate_add(replacement, book);
                if (reason != Reason::None) return reject(reason);
                if (!aggregate_fits(replacement, book, old.price == c.price ? old.remaining_qty : 0))
                    return reject(Reason::QuantityOverflow);
                ++sequence;
                emit(EventType::Amended, replacement, c.quantity, c.quantity);
                if (c.price == old.price && c.quantity <= old.remaining_qty) {
                    h.level->total -= old.remaining_qty - c.quantity;
                    h.order->remaining_qty = c.quantity;
                } else {
                    unlink(h);
                    execute(replacement, book);
                }
            }
        } else return reject(Reason::InvalidCommand);
        return events;
    }
};

Engine::Engine(std::vector<Instrument> instruments, EngineConfig config)
    : impl_(std::make_unique<Impl>(std::move(instruments), config)) {}
Engine::~Engine() = default;
const std::vector<Event>& Engine::process(const Command& c) { return impl_->process(c); }
std::optional<Order> Engine::find(OrderId id) const {
    auto it = impl_->index.find(id);
    return it == impl_->index.end() ? std::nullopt : std::optional<Order>{*it->second.order};
}
Depth Engine::depth(InstrumentId id, std::size_t count) const {
    const auto& b = *impl_->books.at(id);
    Depth d;
    const auto collect = [count](const auto& levels, auto& out) {
        for (const auto& [price, level] : levels) {
            if (count && out.size() == count) break;
            out.push_back({price, level.total, static_cast<std::uint64_t>(level.orders.size())});
        }
    };
    collect(b.bids, d.bids); collect(b.asks, d.asks);
    return d;
}
std::optional<DepthLevel> Engine::best_bid(InstrumentId id) const {
    const auto& levels = impl_->books.at(id)->bids;
    if (levels.empty()) return std::nullopt;
    const auto& level = levels.begin()->second;
    return DepthLevel{level.price, level.total, static_cast<std::uint64_t>(level.orders.size())};
}
std::optional<DepthLevel> Engine::best_ask(InstrumentId id) const {
    const auto& levels = impl_->books.at(id)->asks;
    if (levels.empty()) return std::nullopt;
    const auto& level = levels.begin()->second;
    return DepthLevel{level.price, level.total, static_cast<std::uint64_t>(level.orders.size())};
}
Snapshot Engine::snapshot() const {
    Snapshot s{impl_->sequence, impl_->volume_low, impl_->volume_high, {}, {}};
    s.orders.reserve(impl_->index.size());
    for (const auto& [id, book] : impl_->books) {
        (void)id;
        s.instruments.push_back(book->spec);
        for (const auto& [price, level] : book->bids) {
            (void)price;
            s.orders.insert(s.orders.end(), level.orders.begin(), level.orders.end());
        }
        for (const auto& [price, level] : book->asks) {
            (void)price;
            s.orders.insert(s.orders.end(), level.orders.begin(), level.orders.end());
        }
    }
    return s;
}
std::uint64_t Engine::checksum() const {
    // FNV-1a over explicit little-endian integers, independent of struct padding.
    const auto s = snapshot();
    std::uint64_t hash = 14695981039346656037ULL;
    const auto feed = [&](std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) { hash ^= (value & 0xffU); hash *= 1099511628211ULL; value >>= 8U; }
    };
    feed(1); feed(s.sequence); feed(s.traded_volume_low); feed(s.traded_volume_high);
    feed(s.instruments.size());
    for (const auto& spec : s.instruments) { feed(spec.id); feed(static_cast<std::uint64_t>(spec.tick_size)); }
    feed(s.orders.size());
    for (const auto& o : s.orders) {
        feed(o.id); feed(o.instrument); feed(static_cast<std::uint64_t>(o.side));
        feed(static_cast<std::uint64_t>(o.price)); feed(o.remaining_qty); feed(o.sequence);
    }
    return hash;
}
std::uint64_t Engine::sequence() const { return impl_->sequence; }
std::size_t Engine::active_orders() const { return impl_->index.size(); }
MemoryStats Engine::memory_stats() const {
    return {impl_->logical.allocations, impl_->upstream.allocations, impl_->upstream.bytes, impl_->upstream.peak};
}
std::string Engine::check_invariants() const {
    std::unordered_set<OrderId> seen;
    for (const auto& [id, book] : impl_->books) {
        if (!book->bids.empty() && !book->asks.empty() && book->bids.begin()->first >= book->asks.begin()->first)
            return "Crossed book";
        const auto check = [&, instrument = id, book_ptr = book.get()](const auto& levels, Side side) -> std::string {
            for (const auto& [price, level] : levels) {
                if (level.orders.empty() || level.price != price) return "Empty or mispriced level";
                Quantity total = 0;
                std::uint64_t last_sequence = 0;
                for (auto it = level.orders.begin(); it != level.orders.end(); ++it) {
                    const auto& o = *it;
                    if (!o.remaining_qty || o.price != price || o.instrument != instrument || o.side != side ||
                        o.price <= 0 || o.price % book_ptr->spec.tick_size) return "Invalid resting order";
                    if (!o.sequence || o.sequence <= last_sequence || o.sequence > impl_->sequence)
                        return "FIFO priority violated";
                    last_sequence = o.sequence;
                    if (!seen.insert(o.id).second) return "Duplicate live ID";
                    if (o.remaining_qty > std::numeric_limits<Quantity>::max() - total) return "Quantity overflow";
                    total += o.remaining_qty;
                    auto found = impl_->index.find(o.id);
                    if (found == impl_->index.end() || found->second.book != book_ptr ||
                        found->second.level != &level || &*found->second.order != &o) return "Broken index";
                }
                if (total != level.total) return "Aggregate quantity mismatch";
            }
            return {};
        };
        auto error = check(book->bids, Side::Buy);
        if (!error.empty()) return error;
        error = check(book->asks, Side::Sell);
        if (!error.empty()) return error;
    }
    if (seen.size() != impl_->index.size()) return "Orphaned index entry";
    return {};
}
} // namespace lob
