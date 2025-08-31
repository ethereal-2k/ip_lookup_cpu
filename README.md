# IP Lookup Algorithms on CPU

This project is for experimenting with and benchmarking different IP lookup algorithms on CPU. The focus is on evaluating **lookup speed**, **memory usage**, and **scalability** of common data structures such as **DIR-24-8** and others (to be added later).

The workflow is:
1. Generate a table of prefixes with random cryptographic keys.
2. Generate test IP addresses that fall under those prefixes.
3. Run a lookup algorithm (e.g. DIR-24-8) against the data.
4. Record metrics like build time, lookup throughput, and memory footprint.

## Repository Structure
```
.
├── src/            # Source code (prefix generator, ip generator, algorithms)
├── data/           # Generated CSVs (prefix table, IPs)
├── benchmarks/     # Benchmark results (match files, metrics CSVs, verification scripts)
├── plots/          # Scripts / notebooks for plotting results
└── README.md
```

## 1. Generate Prefix Table
**File:** `src/prefix_gen.cpp`  
Generates `data/prefix_table.csv` containing random prefixes and associated 64-byte keys.

### Compile:
```bash
g++ -O2 -std=c++17 -o src/prefix_gen src/prefix_gen.cpp
```

### Run:
```bash
./src/prefix_gen 10000
```
This creates `data/prefix_table.csv` with 10,000 unique prefixes (default is 10k if no number is provided).

## 2. Generate IP Addresses
**File:** `src/ip_gen.cpp`  
Generates `data/generated_ips.csv` by sampling addresses from the prefix table.

### Compile:
```bash
g++ -O2 -std=c++17 -o src/ip_gen src/ip_gen.cpp
```

### Run:
```bash
./src/ip_gen 1000000
```
This produces 1,000,000 test IP addresses labeled with the prefix they were drawn from.

## 3. Run DIR-24-8 Lookup
**File:** `src/dir24_8.cpp`  
Implements the DIR-24-8 lookup algorithm. It reads the prefix table and IP list, builds the data structure, performs lookups, and records performance metrics.

### Compile:
```bash
g++ -O2 -std=c++17 -o src/dir24_8 src/dir24_8.cpp
```

### Run:
```bash
./src/dir24_8
```

Outputs:
- `benchmarks/match_dir24_8.csv` → mapping of IP → matched key/prefix  
- `benchmarks/results_dir24_8.csv` → benchmark metrics (build time, lookup throughput, memory usage)

## Example Workflow
```bash
# 1. Generate 10k random prefixes
./src/prefix_gen 10000

# 2. Generate 1M IP addresses from those prefixes
./src/ip_gen 1000000

# 3. Build DIR-24-8 and benchmark on the generated data
./src/dir24_8
```

## Benchmark Results
Each run of `dir24_8` appends a row to `benchmarks/results_dir24_8.csv` with columns like:
- `prefix_load_s`, `build_ds_s`, `ip_load_s`, `lookup_s` — timing for each stage
- `lookups_per_s`, `ns_per_lookup` — throughput in lookups/sec and nanoseconds per lookup
- `mem_prefix_array_mb`, `mem_ds_mb`, `mem_ip_array_mb`, `mem_total_mb` — memory usage in MB

## Next Steps
- Add more algorithms (Patricia trie, binary radix trie, Bloom filters)
- Add verification and plotting scripts
- Automate experiments with varying prefix/IP scales