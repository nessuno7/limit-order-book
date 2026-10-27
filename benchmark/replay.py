"""
Replays a workload file (see workload.py) through the Python engine and prints
one JSON line of results. cpp_engine/bench/replay.cpp is the C++ twin; both
emit the same keys so compare.py can line them up.

    python benchmark/replay.py <workload.csv> --mode time   [--reps N] [--dump trades.csv]
    python benchmark/replay.py <workload.csv> --mode memory

time    runs the replay N times; reports median/min wall time and process RSS
memory  runs it once under tracemalloc; reports the peak Python heap growth.
        Separate from `time` because tracing slows the interpreter ~2-3x.

What is timed: everything an engine user pays for -- constructing each Order,
submit(), cancel(), run() -- but not file parsing (timestamps are prebuilt).
"""
import argparse
import json
import statistics
import sys
import time
import tracemalloc
from datetime import datetime, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from python_engine.call_auction.call_auction import CallAuction  # noqa: E402
from python_engine.call_auction.rules.discriminatory import Discriminatory  # noqa: E402
from python_engine.call_auction.rules.uniform import Uniform  # noqa: E402
from python_engine.continuous_auction.continuous_auction import ContinuousDoubleAuction  # noqa: E402
from python_engine.orderbook.order import Order, Side, TimeInForce  # noqa: E402

T0 = datetime(2026, 1, 1, 9, 0, 0)
TRADER = 1
SIDES = {"B": Side.BUY, "S": Side.SELL}
TIFS = {"G": TimeInForce.GTC, "I": TimeInForce.IOC, "F": TimeInForce.FOK}


# ── process memory ───────────────────────────────────────────────────────────

def rss_bytes() -> tuple[int | None, int | None]:
    """(current, peak) resident set size of this process, if the OS says."""
    if sys.platform == "win32":
        import ctypes
        from ctypes import wintypes

        class PMC(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                        ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                        ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]

        pmc = PMC()
        pmc.cb = ctypes.sizeof(PMC)
        k32 = ctypes.WinDLL("kernel32")
        k32.GetCurrentProcess.restype = wintypes.HANDLE
        k32.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(PMC), wintypes.DWORD]
        k32.K32GetProcessMemoryInfo(k32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb)
        return pmc.WorkingSetSize, pmc.PeakWorkingSetSize
    import resource
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    peak = peak if sys.platform == "darwin" else peak * 1024
    try:
        import os
        with open("/proc/self/statm") as f:
            current = int(f.read().split()[1]) * os.sysconf("SC_PAGE_SIZE")
    except OSError:
        current = None
    return current, peak


# ── workload ─────────────────────────────────────────────────────────────────

def load(path: Path):
    lines = path.read_text(encoding="utf-8").splitlines()
    meta = dict(kv.split("=", 1) for kv in lines[0].lstrip("# ").split())
    events = []
    for line in lines[2:]:
        op, idx, side, price, qty, tif, ts = line.split(",")
        if op == "N":
            events.append((op, int(idx), SIDES[side], int(price) if price else None, int(qty), TIFS[tif],
                           T0 + timedelta(microseconds=int(ts))))
        elif op == "C":
            events.append((op, int(idx), None, None, None, None, None))
        else:
            events.append((op, None, None, None, None, None, None))
    return meta, events


def make_engine(meta):
    if meta["kind"] == "continuous":
        return ContinuousDoubleAuction()
    return CallAuction(Uniform() if meta["rule"] == "uniform" else Discriminatory())


def replay(meta, events):
    engine = make_engine(meta)
    orders: list[Order] = []
    append = orders.append
    index = engine.book._index
    for op, idx, side, price, qty, tif, ts in events:
        if op == "N":
            o = Order(side, price, qty, TRADER, tif, timestamp=ts)
            append(o)
            engine.submit(o)
        elif op == "C":
            oid = orders[idx].order_id
            if oid in index:
                engine.book.cancel(oid)
        else:
            engine.run()
    return engine, orders


# ── fingerprints: identical across languages iff the outcomes are ────────────

def fnv1a(text: str) -> str:
    h = 0xcbf29ce484222325
    for b in text.encode():
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def trade_lines(engine, orders) -> list[str]:
    idx = {o.order_id: i for i, o in enumerate(orders)}
    return [f"{idx[t.buy_order_id]},{idx[t.sell_order_id]},{t.price},{t.quantity}\n" for t in engine.trades]


def state_lines(meta, engine, orders) -> list[str]:
    idx = {o.order_id: i for i, o in enumerate(orders)}
    if meta["kind"] == "continuous":
        book, lines = engine.book, []
        for tag, levels in (("B", book.bids), ("S", book.asks)):
            for price in sorted(levels):
                lines += [f"{tag},{price},{idx[oid]},{book._index[oid].quantity}\n" for oid in levels[price]]
        return lines
    return [f"{i},{q}\n" for i, q in sorted((idx[o.order_id], o.quantity) for o in engine._pending)]


def summary(meta, events, engine, orders):
    trades = trade_lines(engine, orders)
    return {
        "events": len(events),
        "orders": len(orders),
        "trades": len(engine.trades),
        "volume": sum(t.quantity for t in engine.trades),
        "notional": sum(t.quantity * t.price for t in engine.trades),
        "trade_digest": fnv1a("".join(trades)),
        "state_digest": fnv1a("".join(state_lines(meta, engine, orders))),
    }, trades


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("workload", type=Path)
    ap.add_argument("--mode", choices=("time", "memory"), default="time")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--dump", type=Path, help="write the trade log here (time mode)")
    args = ap.parse_args()

    meta, events = load(args.workload)
    result = {"language": "python", "scenario": args.workload.stem}
    rss_before, _ = rss_bytes()

    if args.mode == "memory":
        tracemalloc.start()
        engine, orders = replay(meta, events)
        _, peak = tracemalloc.get_traced_memory()
        tracemalloc.stop()
        result["heap_peak_bytes"] = peak
        print(json.dumps(result))
        return

    times, fingerprint, trades = [], None, None
    for _ in range(args.reps):
        t0 = time.perf_counter()
        engine, orders = replay(meta, events)
        times.append(time.perf_counter() - t0)
        fp, trades = summary(meta, events, engine, orders)
        assert fingerprint in (None, fp), "replay is not deterministic"
        fingerprint = fp
        del engine, orders
    _, rss_peak = rss_bytes()

    if args.dump:
        args.dump.parent.mkdir(parents=True, exist_ok=True)
        args.dump.write_text("".join(trades), encoding="utf-8", newline="\n")

    median = statistics.median(times)
    result.update(fingerprint)
    result.update({
        "reps": args.reps,
        "time_s_median": median,
        "time_s_min": min(times),
        "events_per_sec": len(events) / median,
        "ns_per_event": median / len(events) * 1e9,
        "rss_baseline_bytes": rss_before,
        "rss_peak_bytes": rss_peak,
    })
    print(json.dumps(result))


if __name__ == "__main__":
    main()
