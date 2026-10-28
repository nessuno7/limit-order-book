"""
Benchmark for the limit-order-book matching engine.

Measures per-operation latency (ns) and peak memory (bytes) for five workloads.
Outputs a JSON file so results can be compared against a C++ port.

C++ portability notes
---------------------
- Random inputs are generated with seed=42. In C++ use std::mt19937 rng(42)
  with std::uniform_int_distribution to reproduce the same price/qty ranges.
- Workload parameters (N, price ranges, qty ranges) are defined in PARAMS below.
  Keep them identical in the C++ version.
- Output schema: see write_results(). Both implementations should write the same
  keys so a comparison script can diff them directly.
"""

import json
import random
import statistics
import time
import tracemalloc
from pathlib import Path

from python_engine.continuous_auction.continuous_auction import ContinuousDoubleAuction
from python_engine.orderbook.order import Order, Side

# ── Workload parameters ────────────────────────────────────────────────────────
# Keep these identical in any C++ port.
PARAMS = {
    "seed": 42,
    "n_orders": 10_000,       # orders per workload
    "buy_price_range": (80, 99),
    "sell_price_range": (101, 120),
    "match_price": 100,
    "qty_range": (1, 20),
    "n_warmup": 200,           # iterations discarded before timing starts
}

TRADER = 1
RNG = random.Random(PARAMS["seed"])


# ── Helpers ────────────────────────────────────────────────────────────────────

def rand_qty() -> int:
    return RNG.randint(*PARAMS["qty_range"])


def lmt(side: Side, price: int, qty: int) -> Order:
    return Order(side=side, price=price, quantity=qty, trader_id=TRADER)


def time_ns(fn) -> int:
    t0 = time.perf_counter_ns()
    fn()
    return time.perf_counter_ns() - t0


def percentile(data: list[float], p: float) -> float:
    return sorted(data)[int(p * len(data))]


def summarise(name: str, latencies_ns: list[int], peak_bytes: int | None = None) -> dict:
    us = [x / 1_000 for x in latencies_ns]
    result = {
        "workload": name,
        "n": len(us),
        "mean_us":   round(statistics.mean(us), 3),
        "p50_us":    round(percentile(us, 0.50), 3),
        "p95_us":    round(percentile(us, 0.95), 3),
        "p99_us":    round(percentile(us, 0.99), 3),
        "max_us":    round(max(us), 3),
        "throughput_kops_per_sec": round(1_000 / statistics.mean(us), 1),
    }
    if peak_bytes is not None:
        result["peak_memory_bytes"] = peak_bytes
    return result


# ── Workloads ──────────────────────────────────────────────────────────────────

def bench_insert_no_match(n: int) -> dict:
    """
    Submit alternating buy/sell orders that never cross.
    Buys in [80,99], sells in [101,120]. Measures pure OrderBook.add() cost.
    """
    buy_orders  = [lmt(Side.BUY,  RNG.randint(*PARAMS["buy_price_range"]),  rand_qty()) for _ in range(n)]
    sell_orders = [lmt(Side.SELL, RNG.randint(*PARAMS["sell_price_range"]), rand_qty()) for _ in range(n)]
    orders = []
    for b, s in zip(buy_orders, sell_orders):
        orders.append(b)
        orders.append(s)

    # warmup
    e = ContinuousDoubleAuction()
    for o in orders[:PARAMS["n_warmup"]]:
        e.submit(o)

    tracemalloc.start()
    e = ContinuousDoubleAuction()
    latencies = []
    for o in orders:
        latencies.append(time_ns(lambda order=o: e.submit(order)))
    _, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()

    return summarise("insert_no_match", latencies, peak)


def bench_submit_match_1v1(n: int) -> dict:
    """
    Pre-fill n resting sell orders at price=100, qty=1.
    Then submit n buy orders at price=100, qty=1 one at a time.
    Each buy triggers exactly one trade. Measures match + cancel latency.
    """
    # warmup
    for _ in range(PARAMS["n_warmup"]):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.SELL, PARAMS["match_price"], 1))
        e.submit(lmt(Side.BUY,  PARAMS["match_price"], 1))

    tracemalloc.start()
    e = ContinuousDoubleAuction()
    for _ in range(n):
        e.submit(lmt(Side.SELL, PARAMS["match_price"], 1))

    latencies = []
    for _ in range(n):
        latencies.append(time_ns(lambda: e.submit(lmt(Side.BUY, PARAMS["match_price"], 1))))
    _, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()

    return summarise("submit_match_1v1", latencies, peak)


def bench_submit_sweep(n: int) -> dict:
    """
    Each iteration: pre-fill n sell orders spread across n distinct price levels
    [101..100+n], then submit one buy large enough to sweep them all.
    Measures the cost of a full book sweep.
    """
    sweep_qty = n  # one unit per level, aggressor buys all of them

    # warmup
    for _ in range(PARAMS["n_warmup"]):
        e = ContinuousDoubleAuction()
        for p in range(101, 101 + n):
            e.submit(lmt(Side.SELL, p, 1))
        e.submit(lmt(Side.BUY, 100 + n, sweep_qty))

    tracemalloc.start()
    latencies = []

    # We time each full sweep (setup + one aggressor) as one operation.
    # In C++: same structure — refill, then time the aggressor submit.
    for _ in range(max(1, PARAMS["n_warmup"] // 10)):   # fewer reps: each is expensive
        e = ContinuousDoubleAuction()
        for p in range(101, 101 + n):
            e.submit(lmt(Side.SELL, p, 1))
        latencies.append(time_ns(lambda: e.submit(lmt(Side.BUY, 100 + n, sweep_qty))))

    _, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()

    return summarise("submit_sweep", latencies, peak)


def bench_cancel(n: int) -> dict:
    """
    Pre-fill n orders (each at a unique price level so they're all at the
    front of their deque). Cancel them one by one.
    Measures OrderBook.cancel() including level cleanup.
    """
    orders = [lmt(Side.BUY, p, rand_qty()) for p in range(1, n + 1)]

    # warmup
    e = ContinuousDoubleAuction()
    for o in orders[:PARAMS["n_warmup"]]:
        e.submit(o)
    for o in orders[:PARAMS["n_warmup"]]:
        e.book.cancel(o.order_id)

    tracemalloc.start()
    e = ContinuousDoubleAuction()
    for o in orders:
        e.submit(o)

    latencies = []
    for o in orders:
        latencies.append(time_ns(lambda order=o: e.book.cancel(order.order_id)))
    _, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()

    return summarise("cancel", latencies, peak)


def bench_best_price(n: int) -> dict:
    """
    Pre-fill n distinct bid price levels, then call best_bid() repeatedly.
    Exposes the O(n) max() scan — this is the most important scalability signal.
    """
    e = ContinuousDoubleAuction()
    for p in range(1, n + 1):
        e.submit(lmt(Side.BUY, p, 1))

    # warmup
    for _ in range(PARAMS["n_warmup"]):
        e.book.best_bid()

    tracemalloc.start()
    latencies = [time_ns(e.book.best_bid) for _ in range(n)]
    _, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()

    return summarise("best_price", latencies, peak)


# ── Runner ─────────────────────────────────────────────────────────────────────

def run_all() -> list[dict]:
    n = PARAMS["n_orders"]
    print(f"Running benchmarks  n={n}  seed={PARAMS['seed']}\n")

    workloads = [
        ("insert_no_match",   lambda: bench_insert_no_match(n)),
        ("submit_match_1v1",  lambda: bench_submit_match_1v1(n)),
        ("submit_sweep",      lambda: bench_submit_sweep(50)),   # 50 levels per sweep
        ("cancel",            lambda: bench_cancel(n)),
        ("best_price",        lambda: bench_best_price(n)),
    ]

    results = []
    for label, fn in workloads:
        print(f"  {label} ...", end="", flush=True)
        r = fn()
        results.append(r)
        print(f"  mean={r['mean_us']:.1f}µs  p99={r['p99_us']:.1f}µs  "
              f"{r['throughput_kops_per_sec']:.0f}k ops/s"
              + (f"  mem={r.get('peak_memory_bytes', 0) // 1024}KB" if "peak_memory_bytes" in r else ""))

    return results


def write_results(results: list[dict], path: str = "results/python_microbench.json") -> None:
    out = {
        "params": PARAMS,
        "results": results,
    }
    Path(path).parent.mkdir(exist_ok=True)
    with open(path, "w") as f:
        json.dump(out, f, indent=2)
    print(f"\nResults written to {path}")


if __name__ == "__main__":
    results = run_all()
    write_results(results)
