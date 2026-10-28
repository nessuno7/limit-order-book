"""
Turns bench/Sampler.h's address histogram into a flat profile.

    python bench/symbolize.py <exe> <samples.txt> [--top N]

Each sampled address is resolved with `addr2line -f -C -i`, which reports
the chain of inlined functions at that address. Two views are printed:

  self:     the innermost function, where the instruction actually came from
            (std::_Rb_tree::find, an inlined hash lookup, ...)
  function: the outermost one, the function that was really running once
            inlining is accounted for (ContinuousDoubleAuction::submit, ...)
"""
import argparse
import collections
import re
import shutil
import subprocess
import sys


def resolve(exe: str, addresses: list[int]) -> dict[int, list[str]]:
    """address -> inline chain of function names, innermost first."""
    addr2line = shutil.which("addr2line") or "addr2line"
    out = subprocess.run([addr2line, "-a", "-f", "-C", "-i", "-e", exe],
                         input="\n".join(f"0x{a:x}" for a in addresses) + "\n",
                         capture_output=True, text=True, check=True).stdout
    chains: dict[int, list[str]] = {}
    current = None
    lines = out.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if re.fullmatch(r"0x[0-9a-f]+", line):
            current = int(line, 16)  # addr2line zero-pads; compare numerically
            chains[current] = []
            i += 1
            continue
        if current is not None and i + 1 < len(lines):
            chains[current].append(shorten(line))
            i += 2  # function name, then file:line
        else:
            i += 1
    return chains


def shorten(name: str) -> str:
    # Drop template argument lists and parameter lists: enough to tell
    # std::_Rb_tree<...>::_M_lower_bound(...) from IdIndex::find(...).
    prev = None
    while prev != name:
        prev = name
        name = re.sub(r"<[^<>]*>", "", name)
    name = re.sub(r"\(.*\)", "", name)
    return name.strip() or "??"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("samples")
    ap.add_argument("--top", type=int, default=15)
    args = ap.parse_args()

    counts = {}
    with open(args.samples) as f:
        for line in f:
            address, n = line.split()
            counts[int(address, 16)] = int(n)
    total = sum(counts.values())
    chains = resolve(args.exe, list(counts))

    self_time = collections.Counter()
    function_time = collections.Counter()
    for address, n in counts.items():
        chain = [c for c in chains.get(address, []) if c != "??"] \
            or ["[outside the exe: Windows DLLs (heap, kernel)]"]
        self_time[chain[0]] += n
        function_time[chain[-1]] += n

    for title, counter in (("self (innermost inlined function)", self_time),
                           ("function (after inlining)", function_time)):
        print(f"\n{title}: {total} samples")
        for name, n in counter.most_common(args.top):
            print(f"  {100 * n / total:5.1f}%  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
