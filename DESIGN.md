# limit-order-book: design

## In short

This project is a matching engine, the core of an exchange, written twice:
once in Python as the readable reference, and once in C++20 as the fast port.
Both engines run three kinds of market behind one interface:

- **Continuous double auction.** Every order matches immediately against the
  best resting orders, the way a stock exchange trades during the day.
- **Uniform-price call auction.** Orders are collected into a batch, then
  crossed all at once at a single clearing price, the way exchanges open and
  close.
- **Discriminatory (pay-as-bid) call auction.** The orders are batched the
  same way, but each trade prints at the resting order's own limit price.

Each market supports limit and market orders and three time-in-force rules:
GTC (rest until filled), IOC (fill what you can now, drop the rest) and FOK
(fill completely now or not at all). The two engines are checked against each
other: replaying the same order flow through both produces byte-identical
fills. That turns the Python engine into an executable specification for the
C++ one.

---

## Contents

1. [Goals and non-goals](#1-goals-and-non-goals)
2. [Architecture](#2-architecture)
3. [The data model](#3-the-data-model)
4. [The order book](#4-the-order-book)
5. [The continuous double auction](#5-the-continuous-double-auction)
6. [Call auctions and the pricing-rule strategy](#6-call-auctions-and-the-pricing-rule-strategy)
7. [Discriminatory clearing](#7-discriminatory-clearing)
8. [Uniform clearing](#8-uniform-clearing)
9. [Fill-or-kill in a batch: the recursive walk](#9-fill-or-kill-in-a-batch-the-recursive-walk)
10. [Validation and the order lifecycle](#10-validation-and-the-order-lifecycle)
11. [The C++ port](#11-the-c-port)
12. [Complexity](#12-complexity)
13. [Testing strategy](#13-testing-strategy)
14. [Benchmarking methodology](#14-benchmarking-methodology)
15. [Differential fuzzing against a reference book](#15-differential-fuzzing-against-a-reference-book)
16. [FastBook: designing for the latency tail](#16-fastbook-designing-for-the-latency-tail)
17. [Known limitations and future work](#17-known-limitations-and-future-work)

---

## 1. Goals and non-goals

**Goals**

- *One interface, many markets.* A `MatchingEngine` takes orders and returns
  trades. The market type changes how matching happens, never how the engine
  is called.
- *Two independent axes.* Timing (continuous or batched) and pricing (uniform
  or discriminatory) are separate decisions. Pricing is a pluggable strategy,
  so adding a rule never means touching the batching code.
- *Correctness that can be stated.* Every rule the engine promises, such as
  "a FOK fills completely or not at all", is written down as an invariant and
  checked by randomized tests in both languages (section 13).
- *A reference and a fast port with the same behavior.* The Python engine is
  optimized for reading, the C++ engine for speed. They must agree trade for
  trade.

**Non-goals (for now)**

- Networking, persistence, a FIX gateway, multi-instrument routing.
- Concurrency. Both engines are single-threaded by design, as most
  production matching engines are inside a single instrument.
- Self-trade prevention, iceberg/hidden orders, stop orders, order amendment.

---

## 2. Architecture

```
            ┌──────────────────────────── MatchingEngine ───────────────────────────┐
            │  book: OrderBook          trades: [Trade]          submit(order)      │
            └──────────────┬────────────────────────────────────────┬───────────────┘
                           │                                        │
              ContinuousDoubleAuction                        CallAuction
              submit = match now,                    submit = validate + collect
              rest the GTC remainder                 run()  = pricing.clear(batch)
                           │                                        │
                       OrderBook                          PricingRule (strategy)
              bids/asks: price → FIFO of ids            ┌──────────┴──────────┐
              index:     id → Order                  Uniform            Discriminatory
                                                     one price p*       pay-as-bid
                                                          └──────────┬──────────┘
                                                               fok module
                                                    recursive fill-or-kill check
```

| Layer | Python | C++ |
|---|---|---|
| data model | `orderbook/order.py` | `orderbook/Order.h`, `Trade.h`, `Types.h` |
| order storage | the garbage collector | `orderbook/OrderArena.h` |
| book | `orderbook/orderbook.py` | `orderbook/OrderBook.{h,cpp}` |
| engine interface | `engine.py` | `Engine.{h,cpp}` |
| continuous market | `continuous_auction/` | `continuous_auction/` |
| batching | `call_auction/call_auction.py` | `call_auction/CallAuction.{h,cpp}` |
| pricing strategy | `call_auction/pricing_rule.py` | `call_auction/PricingRule.h` |
| FOK feasibility | `call_auction/fok.py` | `call_auction/FillOrKill.{h,cpp}` |
| pricing rules | `call_auction/rules/` | `call_auction/rules/` |

The file tree is mirrored one-to-one on purpose. Each C++ file names the
Python file it ports, so a reader can keep them side by side.

---

## 3. The data model

### Order

| field | type | notes |
|---|---|---|
| `side` | `BUY` / `SELL` | |
| `price` | integer ticks, or none | none means a **market order** |
| `quantity` | positive integer | *mutated in place* as the order fills |
| `trader_id` | integer / `uint64` | carried, not used for matching yet |
| `tif` | `GTC` / `IOC` / `FOK` | defaults to GTC |
| `order_id` | integer counter / `uint64` counter | identity; the book indexes by it |
| `timestamp` | wall-clock instant | time priority |
| `seq` | arrival counter | breaks timestamp ties |

Three decisions here shape everything else.

**Prices are integer ticks, never floats.** Price levels are dictionary or
map keys, and `100.1 + 0.2 != 100.3` in floating point would scatter one
level across several keys. Constructing an order with a non-integer price is
rejected.

**Quantity is mutated in place.** An order *is* its remaining quantity. Both
engines decrement `quantity` through whatever alias they hold: the book's
index, the batch's pending list, or the caller's own variable. They all see
the same object. That's natural in Python (reference semantics) and is
reproduced in C++ with non-owning `Order*` pointers (section 11).

**Priority is `(timestamp, seq)`.** Call auctions order a batch by time, and
discriminatory pricing uses "who was there first" to decide the trade price.
Timestamps alone can tie: clocks are coarse, and callers often pass the same
instant. `seq` is a process-wide counter assigned at construction, so ties
resolve to creation order and every order has a unique, total priority.
Python exposes this as `Order.priority`, C++ as `Order::priority()`.

### Trade

`(price, quantity, buy_order_id, sell_order_id, timestamp)`. A trade always
names both sides explicitly, whichever side was the aggressor.

---

## 4. The order book

```
   asks (sell side)                     index
   ┌───────┬──────────────────┐         ┌──────────┬─────────────┐
   │ 10003 │ [#41] → [#57]    │         │ id #12   │ → Order     │
   │ 10002 │ [#33]            │  best   │ id #33   │ → Order     │
   │ 10001 │ [#12] → [#40]    │◄─ ask   │ ...      │             │
   ├───────┼──────────────────┤         └──────────┴─────────────┘
   │  9999 │ [#8]  → [#21]    │◄─ best bid
   │  9998 │ [#3]             │
   └───────┴──────────────────┘
   bids (buy side)       FIFO queue per level = time priority
```

- **Two maps from price to a FIFO queue of order ids**, one per side. The
  queue order *is* time priority within a level.
- **An index from id to order.** It gives O(1) lookups for matching and
  cancelling, and lets the queues hold small ids instead of whole orders.
- **The book is an index, not an owner.** It never decides how long an order
  lives (section 11).

Operations:

| operation | behavior |
|---|---|
| `add(order)` | append the id to its level's queue (creating the level), index it. A market order has no level and is ignored. Re-adding a resting id raises. |
| `cancel(id)` | remove from the index **and from wherever it sits in its queue**, drop the level if it empties, return the order. Unknown ids raise. |
| `best_bid()` / `best_ask()` | highest bid / lowest ask level and the id at its front. |

The fast path matters for `cancel`. The matcher only ever cancels the front of
the best level, an exhausted resting order, so that case is an O(1)
`popleft`/`pop_front`. A user cancel from the middle of a queue falls back to
a linear search of that one level. An earlier version always removed the
front, whatever id was passed, which corrupted the book on any non-front
cancel.

Both languages keep the levels sorted: C++ in a `std::map` (a red-black
tree), Python in a `sortedcontainers.SortedDict`. The best level is the last
or first key, `rbegin()`/`begin()` in C++ and `peekitem(-1)`/`peekitem(0)` in
Python, so neither scans the book. An earlier Python version used a plain
`dict` and found the best price with `max(self.bids)`, an O(L) scan. That one
choice was worth an 8,700x gap in the micro-benchmarks, which measured the data
structure rather than the language (section 11, *Parity*).

---

## 5. The continuous double auction

`submit(order)` runs to completion before returning:

```
submit(order):
    reject if the id is already resting, or quantity <= 0
    if FOK and not fok_walk(order):  return []           # killed, book untouched
    loop:
        best = best opposing level
        if none:                      rest(order); stop
        if market order or best crosses order's limit:
            fill min(order.qty, resting.qty) at the RESTING price
            remove the resting order if exhausted
            stop if order is filled
        else:                         rest(order); stop
    record and return the trades

rest(order):  only a GTC *limit* order rests; anything else is dropped
```

**Trades print at the resting order's price.** The resting order was there
first and published its price. The aggressor gets price improvement whenever
it crosses deeper than needed, so continuous matching is inherently
discriminatory: one aggressor sweeping three levels gets three prices.

**FOK is decided before any trade.** `fok_walk` sums opposing volume level by
level until the order's limit (a market FOK has no limit) and answers whether
enough is there. Only then does matching start, so a FOK can't partially fill
and leave the book changed. A FOK never rests, which is also why the
continuous FOK check is simple: no other FOK can be sitting in the book.

**Market orders never rest.** A market order has no price to rest at, so its
unfilled remainder is cancelled, as on most real venues. GTC on a market order
therefore behaves like IOC.

**Modify follows the usual exchange rule.** `modify(id, price, quantity)`
amends a resting order. Shrinking it at the same price keeps its place in the
queue, because nobody behind it is disadvantaged. Anything else (a new price,
or more quantity) loses time priority: the order leaves the book, gets a new
timestamp and arrival number, and is submitted again as a GTC order, so a new
price that crosses trades immediately at the resting prices. An unknown id
raises (`KeyError` / `std::out_of_range`), as does a non-positive quantity,
since that is what `cancel` is for.

---

## 6. Call auctions and the pricing-rule strategy

`CallAuction` separates *collecting* orders from *crossing* them:

```
submit(order):   reject duplicates in the batch, spent orders,
                 and anything pricing.validate(order) refuses; then append
run():           buys, sells = split(pending)
                 trades, unfilled = pricing.clear(buys, sells)
                 pending = unfilled          # GTC remainders roll forward
```

The pricing rule is a **strategy object** with two methods:

- `validate(order)` lets a rule refuse an order **at submit time**. Without
  it, an order the rule can't handle would sit in the batch and make every
  later `run()` fail: one bad order would poison the auction forever.
  Discriminatory uses it to refuse market orders.
- `clear(buys, sells) -> (trades, unfilled)` is the whole crossing algorithm.
  It applies fills **to the orders it was given** and returns those same
  objects (with reduced quantities) as `unfilled`.

The second rule is a contract, and it was not always kept. Uniform used to
return *copies*, so after a batch the engine was tracking objects the caller
had never seen. Fills on them were invisible to the caller, and one order id
could name two different objects.

The pending set is mirrored in a hash set of ids, so the duplicate check costs
O(1). An earlier list scan made every `submit` O(batch size), which dominated
the whole auction: 0.83 s of a 0.85 s batch in profiling.

---

## 7. Discriminatory clearing

Pay-as-bid. Each trade prints at the price of whichever side **was in the
batch first**.

```
sort buys  by (price descending, priority)
sort sells by (price ascending,  priority)
take the head of each queue
while both heads exist:
    drop FOK heads that cannot fill        (fok_helper, section 9)
    stop if best buy < best sell           # the book no longer crosses
    price = limit of the older head        # by (timestamp, seq)
    fill min(buy.qty, sell.qty); advance whichever head emptied
unfilled = every GTC order left with quantity > 0 (IOC/FOK dropped)
```

Because both queues are price-sorted, the first non-crossing pair proves that
nothing behind it crosses either, so the loop can stop. Every order that
trades does so at its own limit or better.

Market orders are refused (`validate`). A pay-as-bid trade needs a limit price
to pay, and a market-against-market pair has none.

---

## 8. Uniform clearing

Every trade in the batch prints at **one** price, p\*, chosen to maximize
executed volume.

```
candidates = every distinct limit price in the batch
for each candidate p:
    eligible = market orders + buys with price >= p + sells with price <= p
    simulate matching on COPIES of the eligible orders, oldest first
    record volume(p), surplus(p) = |demand - supply|, and the simulation
p* = argmax volume
     then argmin surplus      (the most balanced price)
     then the highest price   (deterministic)
write the p* simulation's quantities back onto the caller's orders
unfilled = GTC leftovers of that simulation + GTC orders ineligible at p*
```

Why every candidate gets a **full simulation** instead of a quick
`min(demand, supply)` formula: FOK orders. At a given p, a FOK either fills
completely or contributes nothing, so a price's real volume depends on which
FOKs are feasible there. Simulating is the honest way to account for that,
and it's why a FOK can *move* the clearing price (the
`fok_requirement_drives_clearing_price_up` test).

Why copies: each candidate's simulation mutates quantities, and only one
simulation, p\*'s, is real. Copies keep the losers from corrupting the input.
Afterwards the winner's quantities are written back by id, so the caller's
objects reflect what traded.

Why "highest price" on a final tie: it is the rule the test suite documents.
It used to be `random.randint` under a module-level `random.seed(17)`. That
reseeded the *global* RNG for the whole process on import, and made the
outcome depend on how many random numbers other code had drawn first.

Market orders take part at every candidate but add no candidate of their own.
A batch made only of market orders has no price discovery and does not trade.
That is a deliberate, tested rule.

**Cost.** This algorithm is O(P × n) copies per batch, for P candidate prices
and n eligible orders. In the replay benchmark it reaches 53,550 copies per
batch, which dominates the call auction's runtime in both languages. The obvious
optimization, left for later, is to rank prices by `min(demand, supply)` from
prefix sums in O(n log n) and fully simulate only the prices that could still
win once FOKs are accounted for.

---

## 9. Fill-or-kill in a batch: the recursive walk

In a continuous market a FOK only ever faces resting liquidity. In a batch it
can face *other FOKs*, and their liquidity is conditional: a FOK sell for 10
only exists if it gets 10 in total. The shared `fok` module (`fok.py`,
`fok.cpp`) answers one question:

> Can `order` fill completely from the opposite queue, given that some
> opposing orders are themselves all-or-nothing?

```
walk(order, own_side, opp_side, ignore, partial, start_volume):
    volume = start_volume
    for opp in opp_side:
        skip if opp in ignore
        if opp is FOK and opp.qty > what we still need:
            # opp can only fill by also taking volume from OUR side.
            # Ask it, recursively, with our remaining need as its head start,
            # on COPIES of ignore/partial:
            if walk(opp, opp_side, own_side, copy(ignore) + {order}, copy(partial), need):
                return True
            ignore.add(opp)               # infeasible; never count it again
            continue
        stop with False if opp no longer crosses our limit
        take min(need, opp's remaining) and record what is left of opp
        return True once volume >= order.qty
    return False
```

`fok_helper` applies this to the head of a queue: it pops and discards FOKs
until one is feasible, or a non-FOK comes up.

**Why the copies matter: a bug the randomized tests surfaced.** Consider:

```
buys:  A  FOK  4  (oldest)          sells:  B  FOK 10
       D  GTC  5                            C  FOK  6
```

A's walk meets B first. B needs 10, so it recurses: B can get A's 4 plus D's
5, which makes 9 < 10, so B is infeasible. But that failed attempt had marked
D as *consumed*. When A tried C next, D looked used up, so C (needing 6 from
A's 4 plus D) failed too, and A was rejected. **Zero trades**, although
A + D fill C exactly (4 + 2) and A fills completely. Each speculative branch
now works on copies of the bookkeeping, so a failed branch leaves nothing
behind. Both pricing rules shared this bug through duplicated code, and both
now share the one fixed module.

**Worst case.** Every infeasible FOK branch can spawn further branches, so
the walk is exponential in the number of *mutually dependent* FOKs. It has
not been a problem in practice: 40,000 randomized FOK-heavy batches, and
FOK-only batches up to 24 orders, each finish in milliseconds. A production
version would bound the recursion depth, or treat FOK selection as a small
knapsack problem.

---

## 10. Validation and the order lifecycle

```
           ┌──────────── constructed: quantity > 0, integer price ────────────┐
           ▼                                                                  │
   submit ─┬─ continuous ─ matches now ─┬─ filled (quantity 0) ─ done         │
           │                            ├─ GTC limit remainder ─ rests ─ cancel()
           │                            └─ IOC / FOK / market remainder ─ dropped
           │
           └─ call auction ─ validate ─ pending ─ run() ─┬─ filled ─ done
                                                         ├─ GTC remainder ─ pending again
                                                         └─ IOC / FOK remainder ─ dropped
```

Everything that can be rejected is rejected **at the boundary**, with an
exception that names the problem: `ValueError`/`TypeError` in Python,
`std::invalid_argument` in C++.

| input | rejected by |
|---|---|
| quantity <= 0, non-integer price | `Order` constructor |
| submitting an order that is already resting | `ContinuousDoubleAuction.submit` |
| submitting an order already in the batch | `CallAuction.submit` |
| submitting an order that already filled | both `submit`s |
| a market order to a discriminatory auction | `Discriminatory.validate` |
| cancelling an unknown id | `OrderBook.cancel` (`KeyError` / `std::out_of_range`) |

Each row was a real failure before it was a rule. Resubmitting a resting
order duplicated its id in the queue, and the book crashed on the next match.
Resubmitting a filled order printed **zero-quantity trades**, or rested an
empty order.

---

## 11. The C++ port

The C++ engine is a faithful port of the *algorithms*, but four Python habits
had to be translated deliberately.

**Ownership and identity.** Python keeps an order alive while anything refers
to it, and every holder sees the same object. C++ reproduces that with
`OrderArena`: a `std::deque<Order>` that owns the orders, while everything
else (book index, pending batch, caller) holds non-owning `Order*`. A deque
never moves existing elements on `push_back`, so the pointers stay valid. No
per-order `new`/`delete`, no reference counting, no `shared_ptr` control
blocks. Nothing is reclaimed until the arena is destroyed, which is the right
trade for a benchmark and a simple engine. A long-running engine would
recycle slots through a free list.

**Copies for Uniform.** Python's `Order.copy()` copies the eight fields one by
one; C++ uses the defaulted copy constructor, which does the same. Both keep
`orderId` and `seq`, which the write-back and the priority sort rely on. The copies live in an `OrderArena` *local to each `clear()` call*: they
die when it returns, because only the originals escape.

**Ids.** Both languages use a process-wide integer counter, starting at 1.
Only identity matters, and a counter is cheaper to hash and compare than a
UUID, and readable in a debugger.

**Data structures.**

| | Python | C++ |
|---|---|---|
| order | `@dataclass(slots=True)` | plain `struct` |
| levels | `SortedDict[int, deque[int]]` | `std::map<Price, std::deque<OrderId>>` |
| index | `dict[int, Order]` | `std::unordered_map<OrderId, Order*>` |
| best price | `peekitem(-1)` / `peekitem(0)` | `rbegin()` / `begin()` |
| FOK ignore set | `set[int]`, copied per branch | `std::unordered_set<OrderId>`, copied per branch |
| copy for Uniform | `Order.copy()`, field by field | defaulted copy constructor |
| market order | `price=None` | `std::optional<Price>` |
| missing key | `KeyError` | `.at()` throws `std::out_of_range` |
| list.pop(0) | O(n) | `vector::erase(begin())`: O(n), kept for parity |

**Parity.** The point of having two engines is to compare languages, so
the Python is written to do the same work as the C++, step for step: the same
containers (sorted levels, hash index, hash sets), the same copies, the same
O(n) `pop(0)`/`erase(begin())`, the same validation calls. The first Python
version differed in four places that had nothing to do with the language: a
`dict` for levels (O(L) best price, and `sorted()` on every FOK check and every
Uniform candidate), `uuid4()` ids, `copy.copy` through the generic reduce
protocol, and per-instance `__dict__`s. Those were changed to the C++ shape,
and the gap shrank from 26-67x to 17-30x (README, *Python vs C++*). What remains
is the language: interpreted bytecode, boxed integers, tuples for
`priority`, reference counting. The one Python-only cost kept on purpose is
the `isinstance` checks in `Order.__post_init__`, which stand in for what
the C++ compiler enforces through types.

**Types.** `Price` and `Quantity` are *signed* `int64_t`. The algorithms do
`quantity -= n` and compare against 0, so an accidental over-subtraction
should show up as a visible negative number, not wrap to 18 quintillion.
Timestamps are `std::chrono::system_clock::time_point`, the only standard
clock tied to calendar time.

**Strictness.** The C++ builds with `-Wall -Wextra -Wpedantic -Wshadow
-Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor` and is
warning-free in debug and `-O2`. The `-O2` build is what caught a dangling
reference in the test harness: `CHECK_EQ` bound `const auto&` to
`book.bestBid()->first`, a reference into a temporary `std::optional`.

**Build.** `cpp_engine/Makefile` is the command-line build (`lib`, `app`,
`test`, `bench`, `replay`), and `CMakeLists.txt` is the IDE model CLion reads.
Both are kept in sync and produce the same targets.

---

## 12. Complexity

L = price levels on a side, q = orders in one level, n = orders in a batch,
P = distinct limit prices in a batch, k = trades.

| operation | Python | C++ |
|---|---|---|
| `add` (existing / new level) | O(1) / O(log L) | O(log L) |
| `cancel` (front / middle of level) | O(1) / O(q), O(log L) if the level empties | O(log L) / O(log L + q) |
| `best_bid` / `best_ask` | O(1) | O(1) |
| continuous `submit`, k fills | O(k log L) | O(k log L) |
| discriminatory `clear` | O(n log n + k · n) | same |
| uniform `clear` | O(P · n log n) plus copies | same |
| FOK walk | exponential worst case in mutually dependent FOKs | same |

The `k · n` term in Discriminatory comes from building `[head] + rest` for
the FOK check on every iteration, plus `pop(0)` from the front of a list.
Both are linear. Keeping the queues as indices into a sorted array would
remove it.

---

## 13. Testing strategy

Three layers, mirrored in both languages (Python `tests/`, C++
`cpp_engine/tests/`).

1. **Example tests**, the original suite. They pin specific behavior: price-time
   priority, partial fills, the clearing price on hand-built books,
   rationing, chained FOKs.
2. **Edge-case tests** (`test_edge_cases.py`). Each one targets a path the
   example tests walked around: cancelling a non-front order, IOC/FOK
   leftovers, market FOKs, the batch → clear → pending round trip against a
   *real* pricing rule (the originals used stubs), tie-breaks under any RNG
   state, validation, resubmission.
3. **Invariant tests** (`test_invariants.py`, `test_invariants.cpp`).
   Thousands of small *seeded* random scenarios, checked against properties
   rather than outputs:
   - a FOK trades its full quantity or nothing;
   - IOC/FOK never rest or roll over;
   - no order trades more than it asked for;
   - a GTC order is either fully traded or returned with its exact remainder;
   - every trade respects both limits;
   - uniform: one price per batch, and it achieves the best possible volume
     (checked against an independent `min(demand, supply)` oracle when no FOK
     is present);
   - continuous: the book's levels and index agree, no level is empty, and
     the book is never left crossed.

When these were first run against the original engine, **515 of them
failed**. They exposed bugs no example test had reached, including the
recursive-FOK bug in section 9, which a deliberate re-introduction proved the
suite catches.

A fourth check sits across the languages: the **replay benchmark** (next
section) asserts that the two engines produce byte-identical fills and final
state on the same 100,000-event flow.

---

## 14. Benchmarking methodology

There are two complementary benchmarks.

### Micro-benchmarks: `benchmark/bench.py` and `cpp_engine/bench/bench.cpp`

Five isolated operations, with the same parameters and output schema in both
languages: insert without matching, a 1-against-1 match, a 50-level sweep,
cancel, best-price lookup. They report per-operation latency percentiles.
They answer "how fast is each primitive?", but each language draws its own
random numbers, so the inputs differ slightly.

### Replay benchmark: `benchmark/compare.py`

This answers "how do the two engines compare on *the same* realistic work?"

1. `workload.py` writes one deterministic event file per market:
   - **continuous**: 100,000 events. A random-walk mid price, limit and
     market orders, a GTC/IOC/FOK mix, and 15% cancels aimed mostly at recent
     orders.
   - **uniform / discriminatory**: 100 batches of 200 orders, around a
     mean-reverting mid, with coarse timestamps so priority ties occur.
     Unfilled GTC orders roll forward, so the batch being cleared grows to a
     few thousand orders.
2. `replay.py` and `lob_replay` read the same file and perform the same calls.
   The timed region covers everything an engine user pays for (constructing
   each order, `submit`, `cancel`, `run`) but not file parsing.
3. Each engine fingerprints its outcome with an FNV-1a hash over every fill
   `(buy, sell, price, qty)` in order, and over the final book or pending
   batch. `compare.py` requires the fingerprints, trade counts, volume and
   notional to be identical, and diffs the trade logs if they aren't.
4. **Memory** is measured two ways:
   - *heap peak*: the high-water mark of bytes allocated during one replay.
     Python uses `tracemalloc`, in a separate run because tracing slows the
     interpreter. C++ uses a counting global `operator new`, with libstdc++
     linked statically so its internal allocations are counted too.
   - *process peak RSS*: the OS's peak working set, which includes the Python
     interpreter's own baseline.

Results are in [README.md](README.md#python-vs-c) and
`results/replay.{json,md}`.

---

## 15. Differential fuzzing against a reference book

The invariant tests (section 13) check properties. The fuzzer checks
*answers*: every continuous-book implementation must produce exactly the same
fills as an independent oracle, on millions of random operations.

```
                      seeded OpGenerator
                             │  add / cancel / modify
          ┌──────────────────┼───────────────────┬──────────────────┐
          ▼                  ▼                   ▼                  ▼
   ReferenceBook      engine (std::map)    MapBook (pool)    FastBook (array)
    (the oracle)              │                   │                  │
          └──── after EVERY op: status + full fill list + best bid/ask + resting count
                every 1,000 ops and at the end: every resting order, in priority order
```

**The oracle is deliberately naive** (`harness/ReferenceBook.h`). Each side
is one `std::vector` kept sorted best first, FIFO among equal prices.
Insertion finds its slot by linear scan, matching eats the front, cancel
searches. It is written from the matching rules, not from either real book,
and is short enough to check by eye. Slow is the point: there is no
cleverness for a bug to hide in, and an O(n) oracle is still fast enough for
millions of ops on books of thousands of orders.

**The flow is shaped like a market, not noise** (`harness/OpGenerator.h`).
A mid price random-walks. Most limits sit a few ticks behind it, a minority
cross it, and a few large orders sweep several levels. The mix is about
55% adds, 25% cancels, 20% modifies. Market orders and IOC/FOK are included,
cancels and modifies target orders that are actually resting, and 2% of
requests are deliberately invalid (unknown ids, zero quantities) so the error
paths are compared too. Seeds rotate through three profiles:

| profile | shape | why |
|---|---|---|
| tight | ~70 resting orders, frequent trades | the common case |
| deep | ~5,000 orders over hundreds of levels | long queues; bitmap scans across many words |
| volatile | fast-moving mid, limits up to 40 ticks through it | sweeps, FOK kills |

**A divergence report is a reproduction.** On the first mismatch the fuzzer
prints the seed, the op number, the op, both outcomes and the dozen ops
before it, plus the command that replays exactly that prefix.

**The fuzzer is itself tested.** `fuzz/mutation_check.py` injects seven
realistic bugs into `FastBook` one at a time: an off-by-one in FOK
feasibility, a modify that loses priority when it shouldn't, an off-by-one in
the hash table's backward-shift deletion, a ladder scan that skips a level, a
level total not updated, trades priced at the aggressor's limit, and IOC
remainders resting. The fuzzer must report a divergence for each, and does,
between op 2 and op 7,994.

---

## 16. FastBook: designing for the latency tail

The original C++ engine is a faithful port of the Python, and it carries the
Python's shape: a `std::map` of price levels, a `std::deque` of ids per level,
an `unordered_map` from id to a heap-allocated `Order`, and a
`std::vector<Trade>` built for every call. Each of these allocates, and a
profile shows it: about 40% of the engine's time is inside the Windows heap.
`fast/FastBook.h` keeps the exact matching rules (the fuzzer proves it) and
changes every data structure:

```
  price ladder (one per side)                  order pool (preallocated)
  ┌──────────────────────────────┐              ┌────┬────┬────┬────┬────┐
  │ levels[price - minPrice]     │   head/tail  │ #7 │ #3 │free│ #9 │free│ ...
  │  {head, tail, total qty}     │ ───────────▶ └─┬──┴──▲─┴────┴────┴────┘
  ├──────────────────────────────┤                │ next │ prev   (intrusive
  │ bitmap: 1 bit per level,     │                └──────┘         FIFO list)
  │ set while the level has      │
  │ orders                       │              id index: open addressing,
  └──────────────────────────────┘              linear probing, OrderId -> slot
```

- **Flat price ladder.** Levels live in an array indexed by
  `price - minPrice`. Finding a level is an index, not a tree walk. A bitmap
  with one bit per level finds the next non-empty level when the best one
  empties: 64 levels per `countr_zero`/`countl_zero`, so even a sparse ladder
  is scanned in a few instructions. The cost is memory proportional to the
  price range (16 bytes per tick per side) and a fixed range: prices outside
  `[minPrice, maxPrice]` are rejected.
- **Intrusive lists.** Each order node carries its own `prev`/`next` slot
  indices, so a level is just `{head, tail, total}`. Cancelling from the
  middle of a queue is O(1), where the deque needed a linear search. The
  level's running total makes the FOK check O(levels) instead of O(orders).
- **Object pool.** Every node is allocated up front. A LIFO free list hands
  out slots, so the most recently freed, still cache-warm slot is reused
  first.
- **Open-addressing id index.** Linear probing at a load factor of at most
  0.5, and *backward-shift deletion*: instead of leaving tombstones, a delete
  pulls later entries of the probe run back into the hole, so lookups never
  slow down as orders churn. Keys are mixed with the splitmix64 finaliser.
- **No per-call allocation.** Fills go into a buffer reserved at
  construction and are returned as a `std::span`, valid until the next call.

After construction, `FastBook` performs **zero heap allocations** on submit,
cancel and modify. The latency benchmark counts allocations inside every timed
operation to verify it.

**Ablation.** `fast::MapBook` is the same class with a `std::map` ladder: the
pool, intrusive lists and id index, but tree-shaped levels that allocate a
node per new price. Comparing engine → MapBook → FastBook separates what the
allocation-free order storage is worth from what the flat ladder is worth.

**Measured design choices** (README, *Latency*):

- *Size the id table to the live book.* With the table sized for 1M orders
  (25 MB) instead of 64K, the same flow ran at half the throughput: each
  lookup lands on a cold cache line. The engine's `unordered_map` grows with
  the live book, so it gets this right for free.
- *Identity hashing is faster but fragile.* Sequential ids hashed by identity
  sit in adjacent slots, and throughput rose another 33%. But anyone who
  chooses ids can make them collide (a stride equal to the table size), which
  degrades every lookup to a scan. Splitmix stays the default; identity would
  be safe where the engine assigns the ids itself.

**Measuring the tail.** `bench/latency.cpp` times every operation on its own
with a fenced `rdtsc`, calibrated against the wall clock, with the timer's
own cost (~20 ns) subtracted. It pins itself to one core at high priority,
runs five passes interleaved across the three books, and reports the median
of each statistic over the passes, so one pass hit by an interrupt doesn't
decide a number. Windows' own clock ticks at 100 ns, too coarse for
operations that take 30. `bench/gbench.cpp` measures the same books with
Google Benchmark, and `make profile` samples them (`perf` on Linux; on
Windows a small sampling profiler in `bench/Sampler.h`, since MinGW's `gprof`
records call counts but no time).

---

## 17. Known limitations and future work

- **Self-trade prevention.** `trader_id` is carried but never compared, so a
  trader can trade with themselves.
- **Timestamps are naive local time** in Python. They're fine for priority
  within one process, but unsafe across DST changes or machines. UTC would fix
  it.
- **Uniform simulates every candidate price** (section 8). Prefix-sum ranking
  would cut most of that work.
- **The FOK walk is exponential in the worst case** (section 9).
- **The C++ arena never frees.** That's fine for a batch run, but a
  long-running engine needs a free list.
- **Modify exists only in the continuous market**, and there is no stop or
  iceberg order type. Each engine trades one instrument.
- **FastBook needs a fixed price range and capacity** chosen up front, and
  is not wired into the call auctions or the Python comparison.
- See [ROADMAP.md](ROADMAP.md) for what comes next: real market data, then a
  backtester.
