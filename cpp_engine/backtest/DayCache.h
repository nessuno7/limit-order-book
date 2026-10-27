//
// A LOBSTER day compiled for replay.
//
// Re-syncing the book to LOBSTER's orderbook file after every message is
// what keeps it exact, but it is most of the cost of a backtest, and its
// result never changes: the historical book depends only on the feed, never
// on the strategy. So a day is compiled once. compileDay() replays it with
// ReplayBook, records the corrections each re-sync made (about 4% of rows
// need any), and keeps only those and the visible messages. A backtest then
// replays messages and corrections, without the 63 MB orderbook file.
//
// The compiled day is cached in a binary file next to the message file, one
// per (day, price divisor), and rebuilt when either CSV changes (size or
// modification time) or the format version moves.
//

#ifndef LIMIT_ORDER_BOOK_BACKTEST_DAY_CACHE_H
#define LIMIT_ORDER_BOOK_BACKTEST_DAY_CACHE_H

#include "backtest/Feeds.h"
#include "backtest/MarketEvent.h"
#include "backtest/ReplayBook.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace bt {
    struct CompiledDay {
        std::vector<MarketEvent> events;   // the messages that change the visible book, in order
        std::vector<Correction> fixes;     // every recorded correction, in order
        std::vector<std::uint32_t> fixEnd; // events[i]'s fixes end at fixEnd[i + 1]; seeding's at fixEnd[0]
        std::size_t rows = 0;              // lines in the message file, visible or not
        std::size_t depth = 0;             // levels per side in the orderbook file
        Price minPrice = 0;                // every price seen, as in LobsterDay
        Price maxPrice = 0;

        [[nodiscard]] std::span<const Correction> seedFixes() const { return {fixes.data(), fixEnd.front()}; }
        [[nodiscard]] std::span<const Correction> fixesAfter(const std::size_t i) const {
            return {fixes.data() + fixEnd[i], fixes.data() + fixEnd[i + 1]};
        }
    };

    // Replays the day as the backtester does with snapshots (seed from the
    // initial book, then apply and re-sync after each visible message) and
    // records what each re-sync changed. The book is sized as Backtester
    // sizes it, from minPrice - 1 to maxPrice + 1.
    inline CompiledDay compileDay(const LobsterDay &day) {
        CompiledDay c;
        c.rows = day.messages.size();
        c.depth = day.depth;
        c.minPrice = day.minPrice;
        c.maxPrice = day.maxPrice;
        ReplayBook book(day.minPrice - 1, day.maxPrice + 1, 4'096, day.depth);
        book.resyncTo(day.initial, &c.fixes);
        c.fixEnd.push_back(static_cast<std::uint32_t>(c.fixes.size()));
        for (std::size_t i = 0; i < day.messages.size(); ++i) {
            if (!day.messages[i]) continue; // the backtester skips these rows, snapshot included
            c.events.push_back(*day.messages[i]);
            book.apply(*day.messages[i]);
            book.resyncTo(day.books[i], &c.fixes);
            c.fixEnd.push_back(static_cast<std::uint32_t>(c.fixes.size()));
        }
        return c;
    }

    // ── the cache file ───────────────────────────────────────────────────

    // What the cache was built from. Equal fingerprints mean the cache is valid.
    struct DaySource {
        std::uint64_t messageSize = 0, messageTime = 0;
        std::uint64_t bookSize = 0, bookTime = 0;
        std::int64_t priceDivisor = 1;

        bool operator==(const DaySource &) const = default;

        static DaySource of(const std::string &messagePath, const std::string &bookPath, const Price priceDivisor) {
            namespace fs = std::filesystem;
            const auto stamp = [](const std::string &p) {
                return static_cast<std::uint64_t>(fs::last_write_time(p).time_since_epoch().count());
            };
            return {fs::file_size(messagePath), stamp(messagePath), fs::file_size(bookPath), stamp(bookPath),
                    priceDivisor};
        }
    };

    namespace detail {
        inline constexpr char DAY_MAGIC[8] = {'L', 'O', 'B', 'D', 'A', 'Y', '\0', '\0'};
        inline constexpr std::uint32_t DAY_VERSION = 2;

#pragma pack(push, 1)
        struct DayHeader {
            char magic[8];
            std::uint32_t version;
            DaySource source;
            std::uint64_t rows, depth;
            std::int64_t minPrice, maxPrice;
            std::uint64_t events, fixes;
        };

        struct DiskEvent { // MarketEvent without its padding: 34 bytes instead of 48
            std::int64_t time;
            std::uint64_t id;
            std::int64_t price, quantity;
            std::uint8_t type, side;
        };

        struct DiskFix {
            std::uint32_t after; // 0: seeding; i + 1: after events[i]
            std::uint8_t kind, side;
            std::int64_t price, want;
        };
#pragma pack(pop)

        template<class T>
        void writeAll(std::ofstream &out, const std::vector<T> &v) {
            out.write(reinterpret_cast<const char *>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(T)));
        }

        template<class T>
        bool readAll(std::ifstream &in, std::vector<T> &v, const std::uint64_t n) {
            v.resize(n);
            in.read(reinterpret_cast<char *>(v.data()), static_cast<std::streamsize>(n * sizeof(T)));
            return static_cast<bool>(in);
        }
    }

    // Writes to `path` + ".tmp" and renames, so a crash never leaves a torn cache.
    inline void saveDay(const CompiledDay &day, const DaySource &source, const std::string &path) {
        using namespace detail;
        DayHeader h{};
        std::memcpy(h.magic, DAY_MAGIC, sizeof h.magic);
        h.version = DAY_VERSION;
        h.source = source;
        h.rows = day.rows;
        h.depth = day.depth;
        h.minPrice = day.minPrice;
        h.maxPrice = day.maxPrice;
        h.events = day.events.size();
        h.fixes = day.fixes.size();

        std::vector<DiskEvent> events;
        events.reserve(day.events.size());
        for (const MarketEvent &e: day.events) {
            events.push_back({e.time, e.id, e.price, e.quantity, static_cast<std::uint8_t>(e.type),
                              static_cast<std::uint8_t>(e.side)});
        }
        std::vector<DiskFix> fixes;
        fixes.reserve(day.fixes.size());
        for (std::uint32_t group = 0, i = 0; i < day.fixes.size(); ++i) {
            while (i >= day.fixEnd[group]) ++group;
            const Correction &c = day.fixes[i];
            fixes.push_back({group, static_cast<std::uint8_t>(c.kind), static_cast<std::uint8_t>(c.side), c.price, c.want});
        }

        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("cannot write " + tmp);
            out.write(reinterpret_cast<const char *>(&h), sizeof h);
            writeAll(out, events);
            writeAll(out, fixes);
            if (!out) throw std::runtime_error("write failed: " + tmp);
        }
        std::filesystem::remove(path);
        std::filesystem::rename(tmp, path);
    }

    // nullopt if the file is missing, damaged, from another format version,
    // or built from different source files.
    inline std::optional<CompiledDay> loadDay(const std::string &path, const DaySource &expected) {
        using namespace detail;
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        DayHeader h{};
        if (!in.read(reinterpret_cast<char *>(&h), sizeof h)) return std::nullopt;
        if (std::memcmp(h.magic, DAY_MAGIC, sizeof h.magic) != 0 || h.version != DAY_VERSION) return std::nullopt;
        if (!(h.source == expected)) return std::nullopt;

        CompiledDay day;
        day.rows = h.rows;
        day.depth = h.depth;
        day.minPrice = h.minPrice;
        day.maxPrice = h.maxPrice;
        std::vector<DiskEvent> events;
        std::vector<DiskFix> fixes;
        if (!readAll(in, events, h.events) || !readAll(in, fixes, h.fixes)) return std::nullopt;
        if (in.peek() != std::char_traits<char>::eof()) return std::nullopt; // trailing bytes: not ours

        day.events.reserve(events.size());
        for (const DiskEvent &e: events) {
            if (e.type > static_cast<std::uint8_t>(EventType::Execute) || e.side > 1) return std::nullopt;
            day.events.push_back({e.time, static_cast<EventType>(e.type), e.id, static_cast<Side>(e.side), e.price,
                                  e.quantity});
        }
        day.fixes.reserve(fixes.size());
        day.fixEnd.assign(h.events + 1, 0);
        for (std::size_t i = 0; i < fixes.size(); ++i) {
            const DiskFix &f = fixes[i];
            if (f.kind > 1 || f.side > 1 || f.after > h.events) return std::nullopt;
            if (i > 0 && f.after < fixes[i - 1].after) return std::nullopt;
            ++day.fixEnd[f.after]; // a count per group for now
            day.fixes.push_back({static_cast<Correction::Kind>(f.kind), static_cast<Side>(f.side), f.price, f.want});
        }
        for (std::size_t i = 1; i < day.fixEnd.size(); ++i) day.fixEnd[i] += day.fixEnd[i - 1];
        return day;
    }

    // "<message file>.d<divisor>.lobday"
    inline std::string dayCachePath(const std::string &messagePath, const Price priceDivisor) {
        return messagePath + ".d" + std::to_string(priceDivisor) + ".lobday";
    }

    // The compiled day from its cache, or compiled from the CSVs and cached.
    // `fromCache` reports which. With `useCache` false the cache is neither
    // read nor written.
    inline CompiledDay loadLobsterCompiled(const std::string &messagePath, const std::string &bookPath,
                                           const Price priceDivisor, bool &fromCache, const bool useCache = true) {
        const DaySource source = DaySource::of(messagePath, bookPath, priceDivisor);
        const std::string cache = dayCachePath(messagePath, priceDivisor);
        if (useCache) {
            if (auto day = loadDay(cache, source)) {
                fromCache = true;
                return std::move(*day);
            }
        }
        fromCache = false;
        std::ifstream messages(messagePath), books(bookPath);
        if (!messages || !books) throw std::runtime_error("cannot open " + messagePath + " or " + bookPath);
        CompiledDay day = compileDay(loadLobster(messages, books, priceDivisor));
        if (useCache) saveDay(day, source, cache);
        return day;
    }
}

#endif //LIMIT_ORDER_BOOK_BACKTEST_DAY_CACHE_H
