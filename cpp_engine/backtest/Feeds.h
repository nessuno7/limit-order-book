//
// Sources of market-by-order messages for the backtester.
//
//   syntheticFeed(): runs the fuzzer's order flow (harness/OpGenerator.h)
//     through a matching FastBook and reports what that exchange did, message
//     by message, the way a real feed would. Internally consistent by
//     construction, so it doubles as a test of the passive replay path.
//
//   readLobster(): parses a LOBSTER message file
//     (Time,Type,OrderID,Size,Price,Direction per line).
//

#ifndef LIMIT_ORDER_BOOK_BACKTEST_FEEDS_H
#define LIMIT_ORDER_BOOK_BACKTEST_FEEDS_H

#include "backtest/MarketEvent.h"
#include "backtest/ReplayBook.h"
#include "fast/FastBook.h"
#include "harness/OpGenerator.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <istream>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bt {
    // `ops` requests from the generator, spaced by exponential gaps with mean
    // `meanGap`. Requests the exchange would reject produce no messages.
    inline std::vector<MarketEvent> syntheticFeed(const std::uint64_t seed, const std::size_t ops,
                                                  const FlowParams &params = {}, const Nanos meanGap = 1'000'000,
                                                  const Nanos start = 34'200'000'000'000) { // 09:30
        fast::FastBook book(params.minPrice, params.maxPrice, std::min(ops, params.targetDepth) + 16);
        OpGenerator gen(seed, params);
        std::mt19937_64 clockRng(seed ^ 0x9e3779b97f4a7c15ULL);
        std::exponential_distribution<double> gap(1.0 / static_cast<double>(meanGap));
        std::vector<MarketEvent> out;
        Nanos t = start;

        const auto executions = [&](const OrderId taker, const Side takerSide, const std::span<const fast::Fill> fills) {
            const Side makerSide = takerSide == Side::BUY ? Side::SELL : Side::BUY;
            for (const fast::Fill &f: fills) {
                const OrderId maker = f.buyOrderId == taker ? f.sellOrderId : f.buyOrderId;
                out.push_back({t, EventType::Execute, maker, makerSide, f.price, f.quantity});
            }
        };
        const auto restedAdd = [&](const OrderId id) {
            if (const auto r = book.find(id)) out.push_back({t, EventType::Add, id, r->side, r->price, r->quantity});
        };

        for (std::size_t i = 0; i < ops; ++i) {
            const Op op = gen.next(book);
            t += 1 + static_cast<Nanos>(gap(clockRng));
            switch (op.kind) {
                case OpKind::Add: {
                    if (op.quantity <= 0) break;
                    executions(op.id, op.side, book.submit(op.id, op.side, op.price, op.quantity, op.tif));
                    restedAdd(op.id);
                    break;
                }
                case OpKind::Cancel: {
                    const auto r = book.find(op.id);
                    if (!r) break;
                    book.cancel(op.id);
                    out.push_back({t, EventType::Delete, op.id, r->side, r->price, r->quantity});
                    break;
                }
                case OpKind::Modify: {
                    const auto r = book.find(op.id);
                    if (!r || op.quantity <= 0) break;
                    const Price newPrice = *op.price;
                    if (newPrice == r->price && op.quantity <= r->quantity) { // keeps priority
                        if (op.quantity < r->quantity) {
                            out.push_back({t, EventType::Cancel, op.id, r->side, r->price, r->quantity - op.quantity});
                        }
                        book.modify(op.id, newPrice, op.quantity);
                        break;
                    }
                    // Loses priority: the feed shows the old order leaving and a new one arriving.
                    out.push_back({t, EventType::Delete, op.id, r->side, r->price, r->quantity});
                    executions(op.id, r->side, book.modify(op.id, newPrice, op.quantity));
                    restedAdd(op.id);
                    break;
                }
            }
        }
        return out;
    }

    namespace detail {
        // "34200.004241176" -> nanoseconds, without going through a double.
        inline std::optional<Nanos> parseSeconds(const std::string_view s) {
            const std::size_t dot = s.find('.');
            const std::string_view whole = s.substr(0, dot);
            Nanos secs = 0;
            if (std::from_chars(whole.data(), whole.data() + whole.size(), secs).ec != std::errc{}) return std::nullopt;
            Nanos frac = 0;
            if (dot != std::string_view::npos) {
                std::string_view digits = s.substr(dot + 1);
                if (digits.size() > 9) digits = digits.substr(0, 9);
                for (const char c: digits) {
                    if (c < '0' || c > '9') return std::nullopt;
                    frac = frac * 10 + (c - '0');
                }
                for (std::size_t i = digits.size(); i < 9; ++i) frac *= 10;
            }
            return secs * 1'000'000'000 + frac;
        }

        template<class T>
        bool parseInt(const std::string_view s, T &out) {
            return std::from_chars(s.data(), s.data() + s.size(), out).ec == std::errc{};
        }
    }

    // One LOBSTER message line. Types 5 (hidden execution), 6 (cross trade)
    // and 7 (halt) do not change the visible book and return nullopt, as do
    // malformed lines. LOBSTER prices are dollars x 10000; `priceDivisor`
    // rescales them (100 turns them into cents).
    inline std::optional<MarketEvent> parseLobsterLine(std::string_view line, const Price priceDivisor = 1) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
        std::string_view f[6];
        for (int i = 0; i < 6; ++i) {
            const std::size_t comma = line.find(',');
            if (i < 5 && comma == std::string_view::npos) return std::nullopt;
            f[i] = line.substr(0, comma);
            line = comma == std::string_view::npos ? std::string_view{} : line.substr(comma + 1);
        }
        MarketEvent e;
        const auto time = detail::parseSeconds(f[0]);
        int type = 0, direction = 0;
        if (!time || !detail::parseInt(f[1], type) || !detail::parseInt(f[2], e.id) ||
            !detail::parseInt(f[3], e.quantity) || !detail::parseInt(f[4], e.price) ||
            !detail::parseInt(f[5], direction)) {
            return std::nullopt;
        }
        e.time = *time;
        switch (type) {
            case 1: e.type = EventType::Add; break;
            case 2: e.type = EventType::Cancel; break;
            case 3: e.type = EventType::Delete; break;
            case 4: e.type = EventType::Execute; break;
            default: return std::nullopt;
        }
        if (direction != 1 && direction != -1) return std::nullopt;
        e.side = direction == 1 ? Side::BUY : Side::SELL;
        e.price /= priceDivisor;
        return e;
    }

    // One LOBSTER orderbook line: AskPrice1,AskSize1,BidPrice1,BidSize1,...
    // Empty levels (size 0, dummy prices +-9999999999) are left out.
    inline std::optional<TopOfBook> parseLobsterBookLine(std::string_view line, const Price priceDivisor = 1) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
        TopOfBook t;
        for (int column = 0; !line.empty(); column += 2) {
            Price price = 0;
            Quantity quantity = 0;
            for (int k = 0; k < 2; ++k) {
                const std::size_t comma = line.find(',');
                const std::string_view field = line.substr(0, comma);
                if (!(k == 0 ? detail::parseInt(field, price) : detail::parseInt(field, quantity))) return std::nullopt;
                line = comma == std::string_view::npos ? std::string_view{} : line.substr(comma + 1);
                if (k == 0 && comma == std::string_view::npos) return std::nullopt;
            }
            if (quantity <= 0) continue;
            (column % 4 == 0 ? t.asks : t.bids).emplace_back(price / priceDivisor, quantity);
        }
        return t;
    }

    inline std::vector<MarketEvent> readLobster(std::istream &in, const Price priceDivisor = 1) {
        std::vector<MarketEvent> out;
        std::string line;
        while (std::getline(in, line)) {
            if (const auto e = parseLobsterLine(line, priceDivisor)) out.push_back(*e);
        }
        return out;
    }

    // A LOBSTER day: the message file and the orderbook file, line for line.
    // messages[i] is nullopt for lines that do not change the visible book
    // (hidden executions, halts); books[i] is the top of book after line i.
    struct LobsterDay {
        std::vector<std::optional<MarketEvent>> messages;
        std::vector<TopOfBook> books;
        TopOfBook initial; // before the first message
        std::size_t depth = 0; // levels per side in the orderbook file
        Price minPrice = 0; // every price seen, in either file
        Price maxPrice = 0;
    };

    namespace detail {
        // Undo one message on a level snapshot (best first on each side).
        inline void undo(TopOfBook &t, const MarketEvent &e) {
            auto &levels = e.side == Side::BUY ? t.bids : t.asks;
            const Quantity delta = e.type == EventType::Add ? -e.quantity : e.quantity;
            const auto it = std::find_if(levels.begin(), levels.end(), [&](const auto &l) { return l.first == e.price; });
            if (it != levels.end()) {
                it->second += delta;
                if (it->second <= 0) levels.erase(it);
            } else if (delta > 0) {
                const auto worse = std::find_if(levels.begin(), levels.end(), [&](const auto &l) {
                    return e.side == Side::BUY ? l.first < e.price : l.first > e.price;
                });
                levels.insert(worse, {e.price, delta});
            }
        }
    }

    // Throws std::runtime_error if the files differ in length or a line of
    // the orderbook file does not parse.
    inline LobsterDay loadLobster(std::istream &messages, std::istream &books, const Price priceDivisor = 1) {
        LobsterDay day;
        std::string m, b;
        bool any = false;
        const auto see = [&](const Price p) {
            day.minPrice = any ? std::min(day.minPrice, p) : p;
            day.maxPrice = any ? std::max(day.maxPrice, p) : p;
            any = true;
        };
        while (std::getline(messages, m)) {
            if (!std::getline(books, b)) throw std::runtime_error("orderbook file is shorter than the message file");
            if (day.books.empty()) day.depth = static_cast<std::size_t>(std::count(b.begin(), b.end(), ',') + 1) / 4;
            const auto book = parseLobsterBookLine(b, priceDivisor);
            if (!book) throw std::runtime_error("bad orderbook line " + std::to_string(day.books.size() + 1));
            const auto e = parseLobsterLine(m, priceDivisor);
            if (e) see(e->price);
            for (const auto &l: book->bids) see(l.first);
            for (const auto &l: book->asks) see(l.first);
            day.messages.push_back(e);
            day.books.push_back(*book);
        }
        if (std::getline(books, b) && !b.empty()) throw std::runtime_error("orderbook file is longer than the message file");
        if (!day.books.empty()) {
            day.initial = day.books.front();
            if (day.messages.front()) detail::undo(day.initial, *day.messages.front());
        }
        return day;
    }
}

#endif //LIMIT_ORDER_BOOK_BACKTEST_FEEDS_H
