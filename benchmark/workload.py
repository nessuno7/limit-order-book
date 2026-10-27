"""
Deterministic order streams for the Python-vs-C++ replay benchmark.

Both engines replay the *same* file, event by event, so their fills can be
compared byte for byte and their time and memory measured on identical work.

File format (one event per line, after a `# kind=... rule=...` header):

    op,idx,side,price,qty,tif,ts_us

    op     N = new order, C = cancel order #idx (if still resting), R = run batch
    idx    order index: the n-th N line is order n; C lines refer back to one
    side   B / S
    price  integer ticks, empty for a market order
    tif    G / I / F  (GTC / IOC / FOK)
    ts_us  microseconds after a fixed epoch; sets time priority in call auctions

Usage: python benchmark/workload.py [out_dir]   (default benchmark/workloads)
"""
import random
import sys
from pathlib import Path

SEED = 7
MID = 10_000

SCENARIOS = {
    # 100k events of continuous trading: limit/market, GTC/IOC/FOK, and
    # cancels aimed mostly at recent orders.
    "continuous": dict(kind="continuous", rule="", n_events=100_000),
    # 100 call-auction batches of 200 orders each. Unfilled GTC orders roll
    # forward, so the batch the rule clears grows to a few thousand orders.
    "uniform": dict(kind="call", rule="uniform", n_batches=100, batch_size=200, market=0.05),
    "discriminatory": dict(kind="call", rule="discriminatory", n_batches=100, batch_size=200, market=0.0),
}


def _tif(rng, gtc, ioc):
    r = rng.random()
    return "G" if r < gtc else "I" if r < gtc + ioc else "F"


def _continuous(rng, n_events):
    lines, n_orders, mid = [], 0, MID
    for i in range(n_events):
        mid = max(MID - 50, min(MID + 50, mid + rng.choice((-1, 0, 0, 1))))
        if n_orders and rng.random() < 0.15:
            # Cancels are skewed towards recent orders, as real cancels are.
            back = min(int(rng.expovariate(1 / 200)), n_orders - 1)
            lines.append(f"C,{n_orders - 1 - back},,,,,")
            continue
        side = rng.choice("BS")
        if rng.random() < 0.04:
            price = ""
        elif side == "B":
            price = mid + rng.randint(-15, 3)
        else:
            price = mid + rng.randint(-3, 15)
        lines.append(f"N,{n_orders},{side},{price},{rng.randint(1, 100)},{_tif(rng, 0.80, 0.12)},{i * 10}")
        n_orders += 1
    return lines


def _call(rng, n_batches, batch_size, market):
    lines, n_orders, mid = [], 0, MID
    for b in range(n_batches):
        # Mean-reverting mid, so old resting orders eventually cross again.
        mid += rng.choice((-2, -1, 0, 1, 2)) + (MID - mid) // 10
        for _ in range(batch_size):
            side = rng.choice("BS")
            if rng.random() < market:
                price = ""
            elif side == "B":
                price = mid + rng.randint(-10, 4)
            else:
                price = mid + rng.randint(-4, 10)
            # Coarse timestamps within the batch, so priority ties happen.
            ts = b * 1_000_000 + rng.randint(0, batch_size) * 1_000
            lines.append(f"N,{n_orders},{side},{price},{rng.randint(1, 100)},{_tif(rng, 0.75, 0.15)},{ts}")
            n_orders += 1
        lines.append("R,,,,,,")
    return lines


def generate(out_dir: Path) -> dict[str, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    paths = {}
    for name, spec in SCENARIOS.items():
        rng = random.Random(f"{SEED}-{name}")
        if spec["kind"] == "continuous":
            lines = _continuous(rng, spec["n_events"])
        else:
            lines = _call(rng, spec["n_batches"], spec["batch_size"], spec["market"])
        path = out_dir / f"{name}.csv"
        header = f"# kind={spec['kind']} rule={spec['rule']}\nop,idx,side,price,qty,tif,ts_us\n"
        path.write_text(header + "\n".join(lines) + "\n", encoding="utf-8", newline="\n")
        paths[name] = path
    return paths


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "workloads"
    for name, path in generate(out).items():
        print(f"{name:15s} -> {path}")
