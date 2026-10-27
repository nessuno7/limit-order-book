"""
Checks that the fuzzer can actually find bugs.

Injects one realistic bug at a time into fast/FastBook.h, rebuilds lob_fuzz,
and requires the fuzzer to report a divergence. FastBook.h is restored byte
for byte afterwards, even on failure or Ctrl-C.

    python fuzz/mutation_check.py          (from cpp_engine/, after `make release`)

Exit status is the number of mutants the fuzzer missed.
"""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent  # cpp_engine/
HEADER = HERE / "fast" / "FastBook.h"
EXE = HERE / "build" / "release" / ("lob_fuzz_mutant.exe" if os.name == "nt" else "lob_fuzz_mutant")
CXX = os.environ.get("CXX", "g++")
INCLUDES = [f"-I{d}" for d in (".", "orderbook", "continuous_auction", "call_auction", "call_auction/rules")]
FUZZ_ARGS = ["--impl", "fast", "--ops", "300000", "--seeds", "3"]

# name -> (original text, mutated text); each original must occur exactly once
MUTANTS = {
    "FOK kills an exactly fillable order": (
        b"if (available < quantity) {\n", b"if (available <= quantity) {\n"),
    "modify with the same quantity loses priority": (
        b"newPrice == n.price && newQuantity <= n.quantity", b"newPrice == n.price && newQuantity < n.quantity"),
    "id index backward shift off by one": (
        b"i <= j ? (i < k && k <= j)", b"i <= j ? (i <= k && k <= j)"),
    "ask ladder skips the adjacent level": (
        b"return firstAtOrAbove(i + 1);", b"return firstAtOrAbove(i + 2);"),
    "level total not reduced by an in-place modify": (
        b"ladder.find(n.price)->quantity -= n.quantity - newQuantity;", b"(void) ladder;"),
    "trade prints at the aggressor's limit": (
        b"fills.push_back(Buy ? Fill{id, maker.id, p, q} : Fill{maker.id, id, p, q});",
        b"fills.push_back(Buy ? Fill{id, maker.id, limit.value_or(p), q} : Fill{maker.id, id, limit.value_or(p), q});"),
    "IOC remainder rests": (
        b"if (quantity > 0 && tif == TimeInForce::GTC && price.has_value()) rest(id, side, *price, quantity, bids);",
        b"if (quantity > 0 && tif != TimeInForce::FOK && price.has_value()) rest(id, side, *price, quantity, bids);"),
}


def main() -> int:
    original = HEADER.read_bytes()
    eol = b"\r\n" if b"\r\n" in original else b"\n"
    missed = 0
    try:
        for name, (before, after) in MUTANTS.items():
            before, after = before.replace(b"\n", eol), after.replace(b"\n", eol)
            if original.count(before) != 1:
                print(f"SKIPPED  {name}: pattern not found exactly once (FastBook.h changed?)")
                missed += 1
                continue
            HEADER.write_bytes(original.replace(before, after))
            subprocess.run([CXX, "-std=c++20", "-O2", *INCLUDES, "fuzz/fuzz.cpp",
                            "build/release/liblob.a", "-o", str(EXE)], cwd=HERE, check=True)
            run = subprocess.run([str(EXE), *FUZZ_ARGS], cwd=HERE, capture_output=True, text=True)
            report = [line.strip() for line in run.stdout.splitlines() if "DIVERGENCE" in line]
            if run.returncode != 0 and report:
                print(f"CAUGHT   {name}: {report[0]}")
            elif run.returncode != 0:
                print(f"CRASHED  {name}: exit {run.returncode} (caught, but by a crash, not a diff)")
            else:
                print(f"MISSED   {name}")
                missed += 1
    finally:
        HEADER.write_bytes(original)
        EXE.unlink(missing_ok=True)
    return missed


if __name__ == "__main__":
    sys.exit(main())
