# results

Committed outputs of the benchmarks and the fuzzer, so the numbers quoted in
the top-level README can be checked without rerunning anything. Each file is
overwritten by the command next to it. Scratch output (workloads, trade
dumps, backtest runs) goes to `out/`, which is not committed.

| file | what it holds | regenerate with |
|---|---|---|
| `fuzz.txt` | Differential fuzz of every continuous book against `ReferenceBook` | `cd cpp_engine && mingw32-make fuzz OPS=2000000 SEEDS=6 > ../results/fuzz.txt` |
| `latency.json`, `latency.md` | Per-operation latency percentiles and allocation counts for the engine, `MapBook` and `FastBook` | `cd cpp_engine && mingw32-make latency` (the `.md` is the printed table) |
| `gbench.json` | Google Benchmark run over the same books | see "Google Benchmark" in the top-level README, adding `--benchmark_out=results/gbench.json` |
| `replay.json`, `replay.md` | Python vs C++ on identical order flow in all three markets: time, memory, and a fill digest that must match | `python benchmark/compare.py` |
| `python_microbench.json` | Python micro-benchmark: ns per operation and peak memory for five workloads | `make bench` |
| `cpp_microbench.json` | The same five workloads in C++ | `cd cpp_engine && mingw32-make bench` |
