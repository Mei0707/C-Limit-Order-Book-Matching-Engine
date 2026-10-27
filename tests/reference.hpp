#pragma once

#include "lob/types.hpp"
#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>

// Deliberately slow oracle: one flat vector, linear ID lookups, linear best-order
// selection. It shares data types, but no matching or validation implementation.
class ReferenceMatcher {
public:
    explicit ReferenceMatcher(std::vector<lob::Instrument> specs) : specs_(std::move(specs)) {
        std::sort(specs_.begin(), specs_.end(), [](auto a, auto b) { return a.id < b.id; });
    }
    std::vector<lob::Event> process(lob::Command command) {
        using namespace lob;
        events_.clear();
        auto spec = std::find_if(specs_.begin(), specs_.end(), [&](auto s) { return s.id == command.instrument; });
        auto old = find(command.id);
        const auto reject = [&](Reason r) {
            events_.push_back({EventType::Rejected, 0, command.id, 0, command.instrument,
                command.side, command.price, command.quantity, 0, 0, r});
            return events_;
        };
        if (spec == specs_.end()) return reject(Reason::InvalidInstrument);
        if (!command.id) return reject(Reason::InvalidId);
        if (command.kind == CommandType::Add) {
            if (old) return reject(Reason::DuplicateId);
        } else if (command.kind == CommandType::Cancel || command.kind == CommandType::Modify) {
            if (!old || old->instrument != command.instrument) return reject(Reason::UnknownOrder);
        } else return reject(Reason::InvalidCommand);
        if (command.kind == CommandType::Cancel) {
            ++sequence_;
            events_.push_back({EventType::Canceled, sequence_, old->id, 0, old->instrument,
                old->side, old->price, old->remaining_qty, 0, 0, Reason::UserRequested});
            remove(old->id);
            return events_;
        }
        auto incoming = command;
        if (command.kind == CommandType::Modify) {
            incoming.side = old->side; incoming.type = OrderType::Limit; incoming.tif = TimeInForce::GTC;
        }
        if (incoming.side != Side::Buy && incoming.side != Side::Sell) return reject(Reason::InvalidSide);
        if (incoming.type != OrderType::Limit && incoming.type != OrderType::Market) return reject(Reason::InvalidType);
        if (incoming.tif != TimeInForce::GTC && incoming.tif != TimeInForce::IOC && incoming.tif != TimeInForce::FOK)
            return reject(Reason::InvalidTimeInForce);
        if (!incoming.quantity) return reject(Reason::InvalidQuantity);
        if (incoming.type == OrderType::Limit) {
            if (incoming.price <= 0 || incoming.price % spec->tick_size) return reject(Reason::InvalidPrice);
        } else {
            if (incoming.price != 0) return reject(Reason::InvalidPrice);
            if (incoming.tif == TimeInForce::GTC) return reject(Reason::UnsupportedCombination);
        }
        if (incoming.type == OrderType::Limit && incoming.tif == TimeInForce::GTC) {
            auto capacity = std::numeric_limits<Quantity>::max();
            for (auto o : orders_)
                if (o.id != incoming.id && o.instrument == incoming.instrument && o.side == incoming.side && o.price == incoming.price)
                    capacity -= o.remaining_qty;
            if (incoming.quantity > capacity) return reject(Reason::QuantityOverflow);
        }
        ++sequence_;
        events_.push_back({command.kind == CommandType::Add ? EventType::Accepted : EventType::Amended,
            sequence_, incoming.id, 0, incoming.instrument, incoming.side, incoming.price,
            incoming.quantity, incoming.quantity, 0, Reason::None});
        if (old && old->price == incoming.price && incoming.quantity <= old->remaining_qty) {
            for (auto& o : orders_) if (o.id == incoming.id) o.remaining_qty = incoming.quantity;
            return events_;
        }
        if (old) remove(old->id);
        const auto eligible = [&](const Order& o) {
            return o.instrument == incoming.instrument && o.side != incoming.side &&
                (incoming.type == OrderType::Market ||
                 (incoming.side == Side::Buy ? o.price <= incoming.price : o.price >= incoming.price));
        };
        if (incoming.tif == TimeInForce::FOK) {
            auto needed = incoming.quantity;
            for (auto o : orders_) if (eligible(o)) needed -= std::min(needed, o.remaining_qty);
            if (needed) {
                events_.push_back({EventType::Canceled, sequence_, incoming.id, 0, incoming.instrument,
                    incoming.side, incoming.price, incoming.quantity, 0, 0, Reason::FOKUnfilled});
                return events_;
            }
        }
        while (incoming.quantity) {
            auto best = orders_.end();
            for (auto it = orders_.begin(); it != orders_.end(); ++it) {
                if (!eligible(*it)) continue;
                if (best == orders_.end() ||
                    (it->price == best->price ? it->sequence < best->sequence :
                     (incoming.side == Side::Buy ? it->price < best->price : it->price > best->price))) best = it;
            }
            if (best == orders_.end()) break;
            auto qty = std::min(incoming.quantity, best->remaining_qty);
            incoming.quantity -= qty; best->remaining_qty -= qty;
            const auto before = volume_low_;
            volume_low_ += qty;
            volume_high_ += volume_low_ < before ? 1U : 0U;
            events_.push_back({EventType::Trade, sequence_, incoming.id, best->id, incoming.instrument,
                incoming.side, best->price, qty, incoming.quantity, best->remaining_qty, Reason::None});
            if (!best->remaining_qty) orders_.erase(best);
        }
        if (incoming.quantity) {
            if (incoming.type == OrderType::Limit && incoming.tif == TimeInForce::GTC)
                orders_.push_back({incoming.id, incoming.instrument, incoming.side, incoming.price, incoming.quantity, sequence_});
            else events_.push_back({EventType::Canceled, sequence_, incoming.id, 0, incoming.instrument,
                incoming.side, incoming.price, incoming.quantity, 0, 0,
                incoming.type == OrderType::Market ? Reason::MarketExpired : Reason::IOCExpired});
        }
        return events_;
    }
    std::optional<lob::Order> find(lob::OrderId id) const {
        for (auto o : orders_) if (o.id == id) return o;
        return std::nullopt;
    }
    lob::Snapshot snapshot() const {
        auto orders = orders_;
        std::sort(orders.begin(), orders.end(), [](auto a, auto b) {
            if (a.instrument != b.instrument) return a.instrument < b.instrument;
            if (a.side != b.side) return a.side < b.side;
            if (a.price != b.price) return a.side == lob::Side::Buy ? a.price > b.price : a.price < b.price;
            return a.sequence < b.sequence;
        });
        return {sequence_, volume_low_, volume_high_, specs_, std::move(orders)};
    }
    lob::Depth depth(lob::InstrumentId instrument) const {
        lob::Depth d;
        for (auto o : snapshot().orders) {
            if (o.instrument != instrument) continue;
            auto& levels = o.side == lob::Side::Buy ? d.bids : d.asks;
            if (levels.empty() || levels.back().price != o.price) levels.push_back({o.price, 0, 0});
            levels.back().quantity += o.remaining_qty;
            ++levels.back().order_count;
        }
        return d;
    }
private:
    void remove(lob::OrderId id) {
        orders_.erase(std::remove_if(orders_.begin(), orders_.end(), [id](auto o) { return o.id == id; }), orders_.end());
    }
    std::vector<lob::Instrument> specs_;
    std::vector<lob::Order> orders_;
    std::vector<lob::Event> events_;
    std::uint64_t sequence_{}, volume_low_{}, volume_high_{};
};
