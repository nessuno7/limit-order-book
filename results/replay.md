| market | events | trades | identical fills | Python time | C++ time | speed-up | Python heap peak | C++ heap peak | heap ratio | Python RSS peak | C++ RSS peak |
|---|---:|---:|:---:|---:|---:|---:|---:|---:|---:|---:|---:|
| continuous | 100,000 | 56,445 | yes | 675.7 ms | 22.7 ms | **30x** | 20.2 MB | 11.2 MB | **1.8x** | 76.9 MB | 29.3 MB |
| uniform | 20,100 | 9,249 | yes | 10.58 s | 444.8 ms | **24x** | 14.5 MB | 10.8 MB | **1.3x** | 41.8 MB | 21.4 MB |
| discriminatory | 20,100 | 8,862 | yes | 757.7 ms | 44.3 ms | **17x** | 5.5 MB | 2.7 MB | **2.1x** | 35.1 MB | 12.2 MB |
