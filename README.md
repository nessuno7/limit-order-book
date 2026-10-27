# limit-order-book

A matching engine (the core of an exchange) written twice: a readable
**Python** reference and a **C++20** port. Both run three markets behind one
interface (continuous trading, uniform-price call auctions and pay-as-bid call
auctions), with limit/market orders, GTC/IOC/FOK time-in-force, cancel and
modify. On top of that sits **`FastBook`**, a latency-oriented C++ book with
zero heap allocations on the hot path. It is checked against a naive
reference book on millions of random operations.

The full design is in **[DESIGN.md](DESIGN.md)**; what comes next is in
**[ROADMAP.md](ROADMAP.md)**.

## Highlights

- **36 million fuzzed operations, zero divergences.** Every continuous book
  (the engine, `MapBook`, `FastBook`) is run against a deliberately naive
  reference book on 12M random add/cancel/modify ops each. All 1,845,780
  fills and 12,006 full-book snapshots are identical. The fuzzer is itself
  tested: it catches all 7 bugs deliberately injected into `FastBook`.
- **`FastBook` cuts tail latency 11x.** On a 5,000-order book, p99.99 drops
  from 9.6 µs to 0.85 µs, p50 from 150 ns to 40 ns, and throughput rises
  from 4.0M to **25M ops/s**. It makes zero heap allocations per operation,
  counted, against 763 per 1,000 ops for the original engine.
- **Python and C++ agree exactly.** On 140,200 replayed events across all
  three markets, both engines produce the same 74,556 trades and the same
  final state, checked with a hash over every fill.
- **A fair language comparison: C++ is 17–30x faster on the same flow.** The
  Python was first rewritten to use the same data structures as the C++.
  That alone made Python 2.2x faster and shrank a misleading "8,700x" gap to
  the real 46x.
- **Testing by invariant found bugs that 124 passing example tests missed,**
  including a fill-or-kill search that leaked state between branches, so a
  batch that could fill traded **nothing**.

## Correctness

### Differential fuzzing

`fuzz/fuzz.cpp` drives every continuous book with the same seeded op stream
and compares each one with `ReferenceBook`, an oracle written from the
matching rules alone: one sorted `std::vector` per side, linear scans, no
cleverness for a bug to hide in.

- **After every op:** status, the full fill list in order, best bid/ask and
  resting count must match.
- **Every 1,000 ops and at the end:** every resting order, in priority order.
- **The flow is shaped like a market:** a random-walking mid, passive
  depth, crossing orders and sweeps, IOC/FOK/market orders, cancels and
  modifies aimed at live orders, and 2% invalid requests. Seeds rotate between
  a tight book, a deep book (5,000 orders over hundreds of levels) and a
  volatile one.

| book | ops | adds / cancels / modifies | fills, identical | FOKs killed | rejections | full-book checks |
|---|---:|---|---:|---:|---:|---:|
| engine (`std::map`) | 12,000,000 | 6.43M / 3.02M / 2.56M | 1,845,780 | 853,061 | 503,754 | 12,006 |
| `MapBook` | 12,000,000 | same | 1,845,780 | 853,061 | 503,754 | 12,006 |
| `FastBook` | 12,000,000 | same | 1,845,780 | 853,061 | 503,754 | 12,006 |

**Does the fuzzer catch bugs?** `make mutants` injects one bug at a time
into `FastBook` and requires a divergence:

| injected bug | caught at |
|---|---:|
| IOC remainder rests in the book | op 2 |
| trade prints at the aggressor's limit, not the resting price | op 28 |
| ask ladder skips the adjacent price level | op 413 |
| FOK kills an exactly fillable order (`<=` for `<`) | op 613 |
| level total not reduced by an in-place modify | op 1,395 |
| modify with the same quantity loses queue priority | op 1,999 |
| hash table backward-shift deletion off by one | op 7,994 |

### Tests

- Python runs **1,278 tests**, C++ **110 test cases** (debug and `-O2`).
- The C++ builds warning-free under `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wshadow -Wold-style-cast`.
- Both suites have three layers:
  1. **Example tests** pin specific behavior.
  2. **Edge-case tests** target the paths the examples walked around.
  3. **Invariant tests** run thousands of seeded random scenarios against rules
     every correct engine obeys. A FOK fills completely or not at all, no GTC
     volume is lost or invented, the uniform price maximizes volume, and the
     book is never left crossed.
- `FastBook` adds directed tests for its data structures: the hash table
  under churn against `unordered_map`, bitmap scans across 64-level words, and
  pool exhaustion.

Bugs found and fixed, all now covered by tests:

| bug | effect |
|---|---|
| `cancel()` always removed the *front* of the price level | cancelling any other order corrupted the book |
| discriminatory rolled IOC/FOK leftovers into the next batch | "immediate" orders got another chance later; 288 of 400 random batches failed |
| market FOK compared a price against `None` | crashed the continuous engine whenever the book was not empty |
| FOK search reused state from a failed branch | a fillable three-FOK batch traded **nothing** |
| uniform tie-break used `random` under a global `seed(17)` | results depended on import order; one test passed only by luck |
| uniform returned *copies* as the unfilled orders | the caller's objects stopped matching what the engine tracked |
| re-submitting a resting or filled order | duplicate ids in the book, **zero-quantity trades**, empty resting orders |
| no input validation | zero or negative quantities and float prices were accepted |
| O(n) duplicate check in `CallAuction.submit` | 97% of call-auction runtime |

## Latency

All figures: one Windows 11 machine (AMD Zen 2, TSC 1.8 GHz), g++ 15.2 `-O2`.
`make latency` replays 2M ops per scenario through each book. It times every
op on its own with a fenced `rdtsc`, subtracts the timer's ~20 ns, pins
itself to one core, and reports the **median of 5 interleaved passes**.
Allocations are counted inside each timed op.

**Deep book** (~5,000 resting orders over hundreds of levels), all operations:

| book | p50 | p90 | p99 | p99.9 | p99.99 | throughput | allocations / 1,000 ops |
|---|---:|---:|---:|---:|---:|---:|---:|
| engine (`std::map`, deque, `unordered_map`) | 150 ns | 391 ns | 862 ns | 2,805 ns | 9,628 ns | 4.0M ops/s | 763 |
| `MapBook` (pool + intrusive lists, `std::map` ladder) | 80 ns | 170 ns | 361 ns | 691 ns | 2,284 ns | 11.6M ops/s | 45 |
| **`FastBook`** (pool + intrusive lists, flat ladder) | **40 ns** | **120 ns** | **220 ns** | **451 ns** | **852 ns** | **25.0M ops/s** | **0** |

By operation, deep book, p50 / p99 / p99.99:

| operation | engine | `FastBook` |
|---|---|---|
| add, rests | 130 ns / 511 ns / 5.4 µs | 30 ns / 230 ns / 0.65 µs |
| add, trades | 160 ns / 3.5 µs / 26.4 µs | 50 ns / 351 ns / 1.3 µs |
| cancel | 170 ns / 571 ns / 6.6 µs | 40 ns / 160 ns / 0.50 µs |
| modify | 301 ns / 1.3 µs / 14.4 µs | 70 ns / 230 ns / 1.2 µs |

The **shallow book** (~70 orders) tells the same story. All ops p50 / p99 /
p99.99: engine 100 ns / 591 ns / 5.8 µs, `FastBook` 30 ns / 311 ns /
0.80 µs. Throughput is 4.7M against 19.1M ops/s. Full tables are in
`benchmark/latency_results.md` and `.json`. Maximums of 0.5–1 ms appear in
every book and are OS interrupts, not the code.

What the numbers say:

- **The gain is biggest in the tail.** `FastBook` is 3.8x faster at p50 but
  11x at p99.99, and 20x at p99.99 for orders that trade. Allocation is what
  makes the tail: a `malloc` is usually fast and occasionally very slow.
  Removing every allocation removes those outliers.
- **The ablation splits the credit.** Allocation-free order storage (pool,
  intrusive lists, open-addressing index) takes the engine from 4.0M to 11.6M
  ops/s. The flat price ladder takes it from 11.6M to 25M.
- **The profiler agrees.** `make profile` samples the replay. The original
  engine spends **41% of its time in the Windows heap**, then in `std::map`
  walks inside `OrderBook::cancel` and `unordered_map` hashing. `FastBook`
  spends 5% outside its own code. Its largest cost is the id hash table
  (~23%), so that is the next thing to tune.
- **Sizing is a design decision.** With the id table sized for 1M orders
  instead of 64K, the same flow ran at half the throughput, because every
  lookup hits a cold cache line. Hashing sequential ids by identity instead
  of splitmix64 added another 33%. It isn't the default: whoever chooses the
  ids could make them collide on purpose.

**Google Benchmark** (`bench/gbench.cpp`, median of 3 repetitions, wall
time per iteration) agrees:

| benchmark | engine | `MapBook` | `FastBook` |
|---|---:|---:|---:|
| add + cancel, 5,000-order book | 225 ns | 96 ns | 43 ns |
| IOC match + replenish, 100-order book | 575 ns | 112 ns | 46 ns |
| market order sweeping 10 levels (+ rebuild) | 4,811 ns | 1,107 ns | 484 ns |
| best bid + ask | 4.8 ns | 1.2 ns | 1.2 ns |
| deep-flow replay, 200k ops | 49.4 ms | 19.1 ms | 9.2 ms |

## Python vs C++

### Making the comparison fair

The first Python engine lost to C++ partly for reasons unrelated to the
language, so it was rewritten to do the same work as the C++, step for step:

| | before | now (matches C++) |
|---|---|---|
| price levels | `dict` (`max()` scan for best price, `sorted()` per FOK/Uniform pass) | `SortedDict`, like `std::map` |
| order ids | `uuid4()` | integer counter |
| order objects | dataclass with `__dict__` | `@dataclass(slots=True)`, like a struct |
| copies for Uniform | `copy.copy` (generic reduce protocol) | field-by-field `copy()`, like the copy constructor |
| FOK bookkeeping | dict of orders | set of ids |

Same algorithms, same fills. Python became **2.2x faster** in the
continuous and uniform markets.

### Same orders, same fills: the replay benchmark

`benchmark/workload.py` writes one deterministic event file per market, and
both engines replay it. `compare.py` requires matching fingerprints over every
fill and the final state before it reports any timing.

| market | events | trades | identical fills | Python | C++ | speed-up | Python heap | C++ heap | less heap |
|---|---:|---:|:---:|---:|---:|---:|---:|---:|---:|
| continuous | 100,000 | 56,445 | yes | 676 ms | 22.7 ms | **30x** | 20.2 MB | 11.2 MB | 1.8x |
| uniform call | 20,100 | 9,249 | yes | 10.58 s | 445 ms | **24x** | 14.5 MB | 10.8 MB | 1.3x |
| discriminatory call | 20,100 | 8,862 | yes | 758 ms | 44.3 ms | **17x** | 5.5 MB | 2.7 MB | 2.1x |

Median of 3 Python and 10 C++ runs, Python 3.10. Heap is the peak allocated
during one replay (`tracemalloc` for Python, a counting `operator new` for
C++). Process peak RSS is 77 / 42 / 35 MB for Python and 29 / 21 / 12 MB for
C++.

### Per primitive

`benchmark/bench.py` and `cpp_engine/bench/bench.cpp`:

| operation | Python | C++ engine | speed-up |
|---|---:|---:|---:|
| insert, no match | 6.7 µs | 0.107 µs | 63x |
| 1-vs-1 match | 15.2 µs | 0.252 µs | 60x |
| 50-level sweep | 579 µs | 7.8 µs | 74x |
| cancel (empties a level each time) | 6.8 µs | 0.353 µs | 19x |
| best-price lookup (10k levels) | 1.7 µs | 0.037 µs | 46x |

- **The old 8,700x best-price gap was a data-structure artifact.** With a
  `dict`, Python scanned all 10,000 levels. With a sorted container it is
  190x faster, and the gap to C++ falls to 46x, the real language cost.
- **Not everything got faster.** Cancel went from 4.3 to 6.8 µs: each cancel
  in that benchmark empties a level, and deleting from a sorted container
  costs more than from a hash map. The C++ `std::map` pays the same O(log n),
  so this is the honest version of the comparison.
- **Uniform clearing is the most expensive operation in both languages.** It
  simulates every candidate price on copies of the batch. DESIGN.md §8
  describes the prefix-sum shortcut that would remove most of that work.

## Semantics both engines share

- Prices are integer ticks; quantities must be positive.
- Time priority is `(timestamp, arrival order)`, so equal timestamps still
  have a well-defined maker.
- Continuous trades print at the resting order's price.
- Only GTC *limit* orders rest or roll into the next batch. IOC and FOK
  remainders are dropped, and market-order remainders are cancelled.
- A FOK trades its full quantity or nothing, including when its liquidity
  comes from other FOKs in the same batch.
- `modify` keeps queue priority only for a reduction at the same price;
  anything else re-enters the book and may trade.
- Uniform picks the price with the most volume, then the smallest imbalance,
  then the highest price.
- Discriminatory (pay-as-bid) refuses market orders at `submit()`.
- Re-submitting an order that is resting, pending or already filled raises.

## Layout

```
python_engine/                 # the reference engine
├── engine.py                  # MatchingEngine interface
├── __main__.py                # demo: one order flow through all three markets
├── orderbook/                 # OrderBook (SortedDict levels) + Order/Trade/Side
├── continuous_auction/        # ContinuousDoubleAuction (submit, modify)
└── call_auction/              # CallAuction, pricing rules, shared FOK walk

cpp_engine/                    # C++20 port, same structure and semantics
├── fast/FastBook.h            # zero-allocation book: flat ladder, pool, intrusive lists
├── harness/                   # ReferenceBook oracle, op generator, adapters
├── fuzz/                      # differential fuzzer + mutation check
├── bench/                     # bench (micro), replay, latency, gbench, sampler
└── tests/                     # dependency-free test suite

tests/                         # Python example, edge-case and invariant tests
benchmark/                     # Python micro-benchmark, replay workloads, results
```

## Setup

### Python engine

Needs Python 3.10+, `pytest` and `sortedcontainers`. The `Makefile` manages a
`.venv` for you:

```sh
make venv      # create .venv and install dependencies
make run       # run the demo
make test      # run the test suite
make compare   # build the C++ replayer, then run the Python-vs-C++ benchmark
make help      # list available targets
```

Without `make`:

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python -m pytest
python benchmark/compare.py        # needs lob_replay built
```

### C++ engine

Needs GNU make and g++ with C++20. CLion bundles both; from Git Bash:

```sh
export PATH="/c/Program Files/JetBrains/CLion 2026.2/bin/mingw/bin:$PATH"
cd cpp_engine
mingw32-make test                      # test suite (FILTER=name for a subset)
mingw32-make fuzz OPS=2000000 SEEDS=6  # differential fuzzing, every book
mingw32-make mutants                   # prove the fuzzer catches injected bugs
mingw32-make latency                   # latency distributions -> benchmark/latency_results.*
mingw32-make profile PROFILE_IMPL=engine   # perf on Linux, sampling profiler on Windows
mingw32-make bench                     # micro-benchmark -> benchmark/results_cpp.json
mingw32-make replay                    # replayer used by benchmark/compare.py
```

Google Benchmark builds through CMake (it is fetched, so it is opt-in):

```sh
cmake -S . -B build-gbench -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOB_GBENCH=ON
cmake --build build-gbench --target lob_gbench
build-gbench/lob_gbench --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
```

CLion reads `CMakeLists.txt` directly and offers every target: `lob_app`,
`lob_tests`, `lob_fuzz`, `lob_latency`, `lob_bench`, `lob_replay`.
