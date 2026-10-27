TSC: 1.797 GHz, timer overhead 20.0 ns (subtracted)

### tight: shallow book, ~50-100 resting orders (2000000 ops, max depth 95)

| book | operation | count | p50 | p90 | p99 | p99.9 | p99.99 | max | mean |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| engine (std::map) | add, no fill | 930753 | 100 ns | 271 ns | 331 ns | 471 ns | 5130 ns | 1032.5 us | 152 ns |
|  | add, trades | 170415 | 160 ns | 491 ns | 1042 ns | 3647 ns | 17.0 us | 2559.6 us | 313 ns |
|  | cancel | 486187 | 100 ns | 120 ns | 150 ns | 230 ns | 1854 ns | 904.8 us | 100 ns |
|  | modify | 390156 | 180 ns | 411 ns | 741 ns | 1473 ns | 7915 ns | 1051.2 us | 213 ns |
|  | all ops | 1977511 | 100 ns | 331 ns | 591 ns | 1232 ns | 5811 ns | 2559.6 us | 165 ns |
| MapBook (std::map + pool) | add, no fill | 930753 | 80 ns | 170 ns | 321 ns | 541 ns | 892 ns | 868.2 us | 97 ns |
|  | add, trades | 170415 | 70 ns | 210 ns | 381 ns | 581 ns | 942 ns | 762.2 us | 105 ns |
|  | cancel | 486187 | 60 ns | 80 ns | 110 ns | 180 ns | 441 ns | 609.4 us | 64 ns |
|  | modify | 390156 | 90 ns | 170 ns | 260 ns | 451 ns | 852 ns | 731.8 us | 99 ns |
|  | all ops | 1977511 | 70 ns | 160 ns | 301 ns | 521 ns | 852 ns | 885.0 us | 89 ns |
| FastBook (flat array) | add, no fill | 930753 | 40 ns | 150 ns | 371 ns | 511 ns | 872 ns | 954.8 us | 83 ns |
|  | add, trades | 170415 | 50 ns | 160 ns | 361 ns | 561 ns | 892 ns | 871.1 us | 88 ns |
|  | cancel | 486187 | 20 ns | 40 ns | 100 ns | 160 ns | 421 ns | 671.8 us | 33 ns |
|  | modify | 390156 | 50 ns | 80 ns | 160 ns | 291 ns | 491 ns | 456.1 us | 56 ns |
|  | all ops | 1977511 | 30 ns | 130 ns | 311 ns | 481 ns | 802 ns | 1050.9 us | 64 ns |

| book | throughput (untimed replay) | heap allocations in timed ops | per 1,000 ops | rejected ops |
|---|---:|---:|---:|---:|
| engine (std::map) | 4.7 M ops/s | 4228245 | 2114.1 | 22489 |
| MapBook (std::map + pool) | 10.8 M ops/s | 615479 | 307.7 | 22489 |
| FastBook (flat array) | 19.1 M ops/s | 0 | 0.0 | 22489 |

### deep: ~5,000 resting orders over hundreds of levels (2000000 ops, max depth 5001)

| book | operation | count | p50 | p90 | p99 | p99.9 | p99.99 | max | mean |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| engine (std::map) | add, no fill | 975837 | 130 ns | 250 ns | 511 ns | 812 ns | 5360 ns | 932.7 us | 145 ns |
|  | add, trades | 45420 | 160 ns | 802 ns | 3477 ns | 8426 ns | 26.4 us | 1407.4 us | 450 ns |
|  | cancel | 497727 | 170 ns | 291 ns | 571 ns | 952 ns | 6613 ns | 794.2 us | 208 ns |
|  | modify | 478704 | 301 ns | 561 ns | 1343 ns | 4639 ns | 14.4 us | 1050.4 us | 315 ns |
|  | all ops | 1997688 | 150 ns | 391 ns | 862 ns | 2805 ns | 9628 ns | 1416.9 us | 209 ns |
| MapBook (std::map + pool) | add, no fill | 975837 | 70 ns | 160 ns | 331 ns | 541 ns | 1292 ns | 686.7 us | 90 ns |
|  | add, trades | 45420 | 50 ns | 220 ns | 711 ns | 1393 ns | 3917 ns | 513.2 us | 128 ns |
|  | cancel | 497727 | 70 ns | 120 ns | 271 ns | 481 ns | 1353 ns | 741.2 us | 92 ns |
|  | modify | 478704 | 120 ns | 200 ns | 431 ns | 972 ns | 4859 ns | 763.1 us | 138 ns |
|  | all ops | 1997688 | 80 ns | 170 ns | 361 ns | 691 ns | 2284 ns | 763.1 us | 103 ns |
| FastBook (flat array) | add, no fill | 975837 | 30 ns | 120 ns | 230 ns | 451 ns | 651 ns | 770.2 us | 60 ns |
|  | add, trades | 45420 | 50 ns | 130 ns | 351 ns | 731 ns | 1343 ns | 11.7 us | 77 ns |
|  | cancel | 497727 | 40 ns | 60 ns | 160 ns | 391 ns | 501 ns | 573.2 us | 45 ns |
|  | modify | 478704 | 70 ns | 120 ns | 230 ns | 491 ns | 1152 ns | 568.8 us | 77 ns |
|  | all ops | 1997688 | 40 ns | 120 ns | 220 ns | 451 ns | 852 ns | 816.2 us | 60 ns |

| book | throughput (untimed replay) | heap allocations in timed ops | per 1,000 ops | rejected ops |
|---|---:|---:|---:|---:|
| engine (std::map) | 4.0 M ops/s | 1526180 | 763.1 | 2312 |
| MapBook (std::map + pool) | 11.6 M ops/s | 90219 | 45.1 | 2312 |
| FastBook (flat array) | 25.0 M ops/s | 0 | 0.0 | 2312 |

written to ../benchmark/latency_results.json
