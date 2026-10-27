"""
Python vs C++ on the same order flow.

1. Generates the workloads (workload.py): one file per market.
2. Replays each file through both engines, each in its own process:
     python replay.py --mode time    (wall time, RSS)
     python replay.py --mode memory  (tracemalloc heap peak; slow, so separate)
     lob_replay                      (wall time, counted heap peak, RSS)
3. Checks the engines agree: same trade count, volume, notional, and the same
   FNV-1a digest over every fill and over the final book / pending batch. On
   a mismatch the two trade logs are diffed and the first divergence printed.
4. Writes benchmark/replay_results.json and benchmark/replay_results.md.

    python benchmark/compare.py [--py-reps 3] [--cpp-reps 10] [--cpp PATH] [--only NAME]

Build the C++ side first:  cd cpp_engine && mingw32-make replay   (or make replay)
"""
import argparse
import json
import platform
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import workload  # noqa: E402

EXE = ".exe" if sys.platform == "win32" else ""
DEFAULT_CPP = HERE.parent / "cpp_engine" / "build" / "release" / f"lob_replay{EXE}"
OUT = HERE / "out"
CHECK_KEYS = ("events", "orders", "trades", "volume", "notional", "trade_digest", "state_digest")


def run_json(cmd: list[str]) -> dict:
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"{' '.join(cmd)} failed:\n{proc.stderr}")
    return json.loads(proc.stdout.strip().splitlines()[-1])


def first_divergence(a: Path, b: Path) -> str:
    la, lb = a.read_text().splitlines(), b.read_text().splitlines()
    for i, (x, y) in enumerate(zip(la, lb)):
        if x != y:
            return f"trade #{i}: python {x!r} vs cpp {y!r}"
    return f"logs agree for {min(len(la), len(lb))} trades; lengths {len(la)} vs {len(lb)}"


def fmt_bytes(n: float | None) -> str:
    if n is None:
        return "n/a"
    for unit in ("B", "KB", "MB", "GB"):
        if abs(n) < 1024 or unit == "GB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n:.0f} B"
        n /= 1024


def fmt_time(s: float) -> str:
    return f"{s * 1e3:.1f} ms" if s < 1 else f"{s:.2f} s"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--py-reps", type=int, default=3)
    ap.add_argument("--cpp-reps", type=int, default=10)
    ap.add_argument("--cpp", type=Path, default=DEFAULT_CPP)
    ap.add_argument("--only", help="run a single scenario")
    args = ap.parse_args()

    if not args.cpp.exists():
        raise SystemExit(f"{args.cpp} not found -- build it: cd cpp_engine && mingw32-make replay")

    paths = workload.generate(HERE / "workloads")
    rows = []
    ok = True
    for name, path in paths.items():
        if args.only and name != args.only:
            continue
        print(f"{name}: python (time) ...", flush=True)
        py = run_json([sys.executable, str(HERE / "replay.py"), str(path), "--mode", "time",
                       "--reps", str(args.py_reps), "--dump", str(OUT / f"{name}_python.csv")])
        print(f"{name}: python (memory) ...", flush=True)
        py.update(heap_peak_bytes=run_json([sys.executable, str(HERE / "replay.py"), str(path),
                                            "--mode", "memory"])["heap_peak_bytes"])
        print(f"{name}: c++ ...", flush=True)
        OUT.mkdir(exist_ok=True)
        cpp = run_json([str(args.cpp), str(path), str(args.cpp_reps), str(OUT / f"{name}_cpp.csv")])

        same = all(py[k] == cpp[k] for k in CHECK_KEYS)
        ok &= same
        if not same:
            diff = {k: (py[k], cpp[k]) for k in CHECK_KEYS if py[k] != cpp[k]}
            print(f"  MISMATCH {diff}\n  {first_divergence(OUT / f'{name}_python.csv', OUT / f'{name}_cpp.csv')}")
        rows.append({"scenario": name, "identical": same, "python": py, "cpp": cpp})

    result = {
        "machine": {"platform": platform.platform(), "processor": platform.processor(),
                    "python": platform.python_version()},
        "workloads": {k: v for k, v in workload.SCENARIOS.items()},
        "results": rows,
    }
    (HERE / "replay_results.json").write_text(json.dumps(result, indent=2), encoding="utf-8")

    md = ["| market | events | trades | identical fills | Python time | C++ time | speed-up "
          "| Python heap peak | C++ heap peak | heap ratio | Python RSS peak | C++ RSS peak |",
          "|---|---:|---:|:---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for r in rows:
        p, c = r["python"], r["cpp"]
        md.append(
            f"| {r['scenario']} | {p['events']:,} | {p['trades']:,} | {'yes' if r['identical'] else '**NO**'} "
            f"| {fmt_time(p['time_s_median'])} | {fmt_time(c['time_s_median'])} "
            f"| **{p['time_s_median'] / c['time_s_median']:.0f}x** "
            f"| {fmt_bytes(p['heap_peak_bytes'])} | {fmt_bytes(c['heap_peak_bytes'])} "
            f"| **{p['heap_peak_bytes'] / c['heap_peak_bytes']:.1f}x** "
            f"| {fmt_bytes(p['rss_peak_bytes'])} | {fmt_bytes(c['rss_peak_bytes'])} |")
    table = "\n".join(md)
    (HERE / "replay_results.md").write_text(table + "\n", encoding="utf-8")
    print("\n" + table)
    if not ok:
        raise SystemExit("engines disagree -- see MISMATCH above")


if __name__ == "__main__":
    main()
