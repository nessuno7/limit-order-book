//
// Records a backtest for inspection (the localhost viewer in ui/): the touch,
// our resting quotes, position and PnL sampled on a grid of feed time, and
// every fill. It wraps the strategy it records, so any strategy can be traced
// without changing it.
//
// Samples describe the exchange's side, not the strategy's possibly delayed
// view: the book and our orders as they stand at the sample time, and PnL
// marked to the exchange's mid.
//
// Sampling can be limited to a window of feed time. A replay is fast and
// deterministic, so a viewer zoomed into a few seconds re-runs the backtest
// and samples just those seconds finely, instead of storing a fine grid for
// the whole day.
//

#ifndef LIMIT_ORDER_BOOK_BACKTEST_TRACE_H
#define LIMIT_ORDER_BOOK_BACKTEST_TRACE_H

#include "backtest/Backtester.h"

#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bt {
    class Trace : public Strategy {
    public:
        struct Sample {
            Nanos time = 0;
            std::optional<Price> bid, ask;
            Quantity bidQuantity = 0, askQuantity = 0;
            std::optional<Price> ourBid, ourAsk; // our best live quote on each side
            Quantity position = 0;
            double pnl = 0.0;                    // ticks, marked to mid, after fees
        };

        struct Fill {
            SimFill fill;
            Quantity position = 0; // after this fill
        };

        // `interval` is the sampling step in feed time; samples are taken in
        // [from, to] only.
        Trace(Strategy &traced, const Nanos interval, const Nanos from = 0,
              const Nanos to = std::numeric_limits<Nanos>::max())
            : inner(traced), step(interval > 0 ? interval : 1), next(from), last(to) {}

        void onEvent(Backtester &b, const MarketEvent &e) override {
            inner.onEvent(b, e);
            if (b.now() >= next && b.now() <= last) {
                record(b);
                next = (b.now() / step + 1) * step;
            }
        }

        void onFill(Backtester &b, const SimFill &f) override {
            position += f.side == Side::BUY ? f.quantity : -f.quantity;
            fills_.push_back({f, position});
            inner.onFill(b, f);
        }

        // A last sample, after Backtester::finish().
        void finish(const Backtester &b) {
            if (b.now() <= last) record(b);
        }

        [[nodiscard]] const std::vector<Sample> &samples() const { return samples_; }
        [[nodiscard]] const std::vector<Fill> &fills() const { return fills_; }

        // series.csv and fills.csv. Missing prices are empty fields.
        void write(const std::string &dir) const {
            File series(dir + "/series.csv");
            std::fprintf(series.f, "time,bid,ask,bid_qty,ask_qty,our_bid,our_ask,position,pnl\n");
            for (const Sample &s: samples_) {
                std::fprintf(series.f, "%lld,%s,%s,%lld,%lld,%s,%s,%lld,%.2f\n", static_cast<long long>(s.time),
                             price(s.bid).c_str(), price(s.ask).c_str(), static_cast<long long>(s.bidQuantity),
                             static_cast<long long>(s.askQuantity), price(s.ourBid).c_str(), price(s.ourAsk).c_str(),
                             static_cast<long long>(s.position), s.pnl);
            }
            File fills(dir + "/fills.csv");
            std::fprintf(fills.f, "time,order,side,price,quantity,maker,position\n");
            for (const Fill &x: fills_) {
                const SimFill &f = x.fill;
                std::fprintf(fills.f, "%lld,%llu,%s,%lld,%lld,%d,%lld\n", static_cast<long long>(f.time),
                             static_cast<unsigned long long>(f.id), f.side == Side::BUY ? "buy" : "sell",
                             static_cast<long long>(f.price), static_cast<long long>(f.quantity), f.maker ? 1 : 0,
                             static_cast<long long>(x.position));
            }
        }

    private:
        struct File {
            explicit File(const std::string &path) : f(std::fopen(path.c_str(), "w")) {
                if (f == nullptr) throw std::runtime_error("cannot write " + path);
            }
            ~File() { std::fclose(f); }
            File(const File &) = delete;
            File &operator=(const File &) = delete;
            std::FILE *f;
        };

        static std::string price(const std::optional<Price> p) { return p ? std::to_string(*p) : std::string(); }

        void record(const Backtester &b) {
            const fast::FastBook &book = b.exchangeBook().fastBook();
            Sample s;
            s.time = b.now();
            s.bid = book.bestBid();
            s.ask = book.bestAsk();
            if (s.bid) s.bidQuantity = book.levelQuantity(Side::BUY, *s.bid);
            if (s.ask) s.askQuantity = book.levelQuantity(Side::SELL, *s.ask);
            for (const SimOrder &o: b.exchangeOrders()) {
                if (!o.live || o.remaining <= 0) continue;
                std::optional<Price> &best = o.side == Side::BUY ? s.ourBid : s.ourAsk;
                if (!best || (o.side == Side::BUY ? o.price > *best : o.price < *best)) best = o.price;
            }
            s.position = b.stats().position;
            s.pnl = b.stats().pnl(b.exchangeMid());
            if (!samples_.empty() && samples_.back().time == s.time) samples_.back() = s;
            else samples_.push_back(s);
        }

        Strategy &inner;
        Nanos step;
        Nanos next;
        Nanos last;
        Quantity position = 0; // from the fills seen, which may lag the exchange's
        std::vector<Sample> samples_;
        std::vector<Fill> fills_;
    };
}

#endif //LIMIT_ORDER_BOOK_BACKTEST_TRACE_H
