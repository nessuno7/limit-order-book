# Roadmap

Where the project goes after the two engines. Each part builds on the one
before: prove the book is right, make it fast and measure it honestly, run it
on real market data, then build a system on top.

| part | goal | status |
|---|---|---|
| 1 | Correctness: differential fuzzing against a naive reference book | **done** |
| 2 | Latency: distributions, a zero-allocation book, profiling | **done** |
| 3 | Real data: rebuild books from Nasdaq ITCH / LOBSTER and check them | planned |
| 4 | A market-making backtester with queue-position modelling | **in progress** |

---

## Part 1: Correctness, done

> Fuzz it against a naive reference book (a simple sorted vector) with
> millions of random add/cancel/modify ops, and require identical fills.

What exists:

- **`modify`** in both engines (C++ and Python), with the usual exchange rule:
  a reduction at the same price keeps queue priority, anything else
  re-enters and may trade. Tested in both languages.
- **`harness/reference_book.h`**: the oracle. One sorted `std::vector` per
  side, linear scans only, written from the rules rather than from either
  implementation.
- **`fuzz/fuzz.cpp`** (`make fuzz`): seeded random add/cancel/modify flow
  in three market shapes (tight, deep, volatile). After every op it compares
  the outcome, every fill, best bid/ask and resting count. Every 1,000 ops and
  at the end it compares the whole book, order by order. A failure prints the
  seed, the op and a replay command.
- **`fuzz/mutation_check.py`** (`make mutants`): proves the fuzzer has teeth
  by injecting seven realistic bugs into `FastBook`; each must be caught.

Result: **12 million ops per implementation, three implementations,
1,845,780 fills, all identical to the reference**, plus 12,006 full-book
comparisons. All seven injected bugs are caught, the slowest after 7,994 ops.

Possible extensions: run the Python engine against the same oracle on a
smaller op count (through a recorded op file); fuzz the call auctions
against a brute-force clearing oracle; add test-case shrinking so a
divergence is reported as the shortest failing prefix.

## Part 2: Latency, done

> Report p50/p99/p99.9 per operation, not an average. Try flat arrays indexed
> by price tick instead of std::map, intrusive lists per price level, object
> pools, and zero allocations in the hot path. Profile with perf and Google
> Benchmark.

What exists:

- **`fast/FastBook.h`**: flat price-tick ladder with a bitmap of non-empty
  levels, intrusive FIFO lists, a preallocated object pool with a LIFO free
  list, an open-addressing id index with backward-shift deletion, and fills
  returned as a span. **Zero heap allocations** on submit/cancel/modify after
  construction, verified by counting.
- **`fast::MapBook`**: the same book on a `std::map` ladder, so the gain of
  each idea can be separated (ablation).
- **`bench/latency.cpp`** (`make latency`): per-op `rdtsc` timing, pinned
  thread, 5 interleaved passes, median of each statistic. Reports p50 / p90 /
  p99 / p99.9 / p99.99 / max per operation type, plus throughput and
  allocations, for the shallow and deep books.
- **`bench/gbench.cpp`**: Google Benchmark suite (CMake, `-DLOB_GBENCH=ON`).
- **`make profile`**: `perf record` on Linux. On Windows, where there is no
  perf and MinGW's gprof takes no time samples, `bench/sampler.h` is a small
  sampling profiler and `bench/symbolize.py` resolves the samples.

Results are in the README under *Latency*. Measured findings worth keeping:
the original engine spends about 40% of its time in the heap allocator;
sizing the id table to the live book doubles throughput; identity hashing
would add a further 33% for sequential ids, at a robustness cost.

Not done here, and why:

- **`perf` numbers.** This machine is Windows without a WSL distribution.
  The Makefile target is ready; run `make profile` on Linux.
- **Hardware counters** (cache misses, branch misses) need perf or VTune.

Next candidates, in order of expected payoff:

1. Make the id hash a policy and offer identity hashing for exchange-assigned
   ids (measured +33%).
2. Pack `Node` from 40 to 32 bytes (`uint32` quantities would do it), so
   exactly two orders share a cache line instead of straddling one.
3. Huge pages for the ladder and pool, to cut TLB misses on deep books.
4. Batch the FOK pre-check into the matching loop when the first level
   already fills the order.

## Part 3: Real data, planned

> Replay Nasdaq TotalView-ITCH sample files or LOBSTER data, rebuild the book,
> and check it against their snapshots. "Processes N million real messages
> per second."

Plan:

1. **A book-rebuilding mode.** A market-data feed reports what the exchange
   already did, so rebuilding a book is not matching. It is applying
   add / execute / cancel(partial) / delete / replace to resting orders by
   id. `FastBook` gets a passive API for that, reusing the ladder, pool and
   id index. Executions reduce the resting order named in the message
   instead of crossing.
2. **LOBSTER first.** Its message file (time, type 1-7, order id, size,
   price, direction) maps directly onto that API. It ships an orderbook file
   with the top N levels after *every* message, which is a ready-made oracle:
   compare after each message, require equality.
3. **ITCH 5.0 next.** A binary parser (big-endian, length-prefixed) for
   `A`/`F` add, `E`/`C` executed, `X` cancel, `D` delete and `U` replace,
   one book per stock locate code. Nasdaq publishes sample days. Validate by
   cross-checking traded volume per stock against the `P`/`Q` trade
   messages, and against LOBSTER for days both cover.
4. **Report** messages per second end to end (parse + apply), split by
   message type, with the same latency distributions as Part 2.

Data note: LOBSTER sample files and Nasdaq's ITCH samples are free to
download, but neither is committed here. A script will fetch them into an
ignored `data/` directory.

## Part 4: Backtester, in progress

> A market-making backtester with queue-position modelling turns it from a
> data structure into a system.

First slice, on branch `backtester` (`cpp_engine/backtest/`, `make backtest`):

- **`MarketEvent.h`**: one market-by-order message, in LOBSTER's vocabulary
  (add, partial cancel, delete, execute), each carrying the side and price
  of the order it names.
- **FastBook passive mode** (Part 3, step 1): `restPassive`, `reduce`,
  `find`, `levelQuantity`, `forEachLevel`. Rebuilds a book from a feed
  without matching.
- **`Feeds.h`**: `syntheticFeed()` runs the fuzzer's flow through a matching
  FastBook and reports what it did as feed messages. `readLobster()` parses
  LOBSTER message files. A test checks that replaying the synthetic feed
  passively rebuilds the generating book exactly, order by order.
- **`Backtester.h`**: the historical book is replayed unchanged; the
  strategy's orders live in a simulated queue beside it.
- **`Strategies.h`**: `QuoteAtTouch`, which joins both sides of the touch,
  follows it and stops at a position limit. It is a baseline, not a strategy.

How the open questions were settled:

- **Queue position is exact, not modelled.** A market-by-order feed names
  the order behind every cancel and execution, and each historical order gets
  an arrival number. Only volume that arrived before us shrinks our queue
  position. Orders resting before the feed began count as ahead of everyone.
  The pessimistic / optimistic / proportional models matter only for
  level-aggregated data.
- **When we fill.** We fill when the feed proves an aggressor got to us:
  1. an execution of an order behind us at our price, or at any worse price;
  2. an incoming order that crosses our price;
  3. our own order crossing the book on arrival (taker, at the resting prices).
- **No market impact (for now).** Our fills don't remove historical
  liquidity, so one aggressor can fill both us and the order it really hit.
  This is optimistic, and acceptable only while our size is small next to the
  book's.
- **Latency.** A constant order-entry delay for placements and cancels, so an
  order can fill while its cancel is in flight. Market data is not delayed yet.
- **Metrics.** Position, cash, fees or rebates, mark-to-mid PnL, maker and
  taker volume, maximum inventory, and markout per share after a horizon
  (adverse selection).

The synthetic feed's results are not meaningful PnL. Its flow is random, so
nobody trades against us with information, and markouts come out positive.
Real data is needed before any strategy number means anything.

Next, roughly in order:

1. ~~**Real data.**~~ Done for the LOBSTER AMZN 2012-06-21 samples (unzipped
   into the ignored `data/lobster/`). `ReplayBook` holds the volume the feed
   never introduced as per-level placeholders, which are seeded and re-synced
   from the orderbook file. When a level leaves view, its orders are folded
   into the placeholder. `make lobster-check` compares the book with
   LOBSTER's snapshot after every message, before re-syncing. On the 10-level
   file:
   - the touch and the top 9 levels match on all 269,748 rows;
   - 0 messages are left unexplained;
   - all 10 levels match on 96.1% of rows. The rest is a level the feed never
     described coming into view at depth 10.
2. **Market-data latency.** The strategy should see the book as it was
   `mdLatency` ago, not as it is now.
3. **A market-impact option.** Let our fills consume the aggressor's volume:
   group a burst of executions with the same timestamp into one aggressor,
   fill us from it, and shrink what reaches the historical orders behind us.
4. **Strategy interface.** Add IOC orders, modify, and queue-position queries
   (`ahead`), plus an equity curve and per-period PnL in the output.
5. **Better baselines.** An inventory-skewed quoter and Avellaneda-Stoikov,
   compared on the same days.
