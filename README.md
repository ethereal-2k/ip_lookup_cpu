# IP Lookup Algorithms on CPU

This project implements and benchmarks multiple IP lookup algorithms for longest prefix match (LPM) on IPv4 addresses. The focus is on evaluating **lookup speed**, **memory usage**, **scalability**, and **dynamic update performance** under realistic workloads. Each algorithm performs LPM lookups and returns 64-byte cryptographic keys for packet source authentication.

## Implemented Algorithms

The project includes five IP lookup algorithms:

1. **Binary Radix Trie** - A straightforward bitwise trie that moves one bit per level
2. **Patricia Trie** - A compressed radix trie that merges long chains of single-child nodes
3. **DIR-24-8** - A two-level lookup table that directly indexes the upper 24 bits
4. **DXR (Dynamic Exact Range)** - Replaces subtables with compact vectors of prefix ranges
5. **DXR Bloom** - DXR enhanced with Bloom filters for fast negative lookups

## Workflow

1. Generate a table of prefixes with random cryptographic keys.
2. Generate test IP addresses that fall under those prefixes.
3. Run lookup algorithms against the data.
4. Record metrics like build time, lookup throughput, memory footprint, and operation costs.
5. Verify correctness and generate performance plots.

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

## 3. Run Lookup Algorithms

All algorithms follow a similar pattern: they read the prefix table and IP list, build the data structure, perform lookups, and record performance metrics.

### Binary Radix Trie
**File:** `src/binary_radix_trie.cpp`
```bash
g++ -O2 -std=c++17 -o src/binary_radix_trie src/binary_radix_trie.cpp
./src/binary_radix_trie
```
Outputs: `benchmarks/match_radix.csv`, `benchmarks/results_radix.csv`

### Patricia Trie
**File:** `src/patricia_trie.cpp`
```bash
g++ -O2 -std=c++17 -o src/patricia_trie src/patricia_trie.cpp
./src/patricia_trie
```
Outputs: `benchmarks/match_pat.csv`, `benchmarks/results_pat.csv`

### DIR-24-8
**File:** `src/dir_24_8.cpp`
```bash
g++ -O2 -std=c++17 -o src/dir_24_8 src/dir_24_8.cpp
./src/dir_24_8
```
Outputs: `benchmarks/match_dir24_8.csv`, `benchmarks/results_dir24_8.csv`

### DXR
**File:** `src/dxr.cpp`
```bash
g++ -O2 -std=c++17 -o src/dxr src/dxr.cpp
./src/dxr
```
Outputs: `benchmarks/match_dxr.csv`, `benchmarks/results_dxr.csv`

### DXR Bloom
**File:** `src/dxr_bloom.cpp`
```bash
g++ -O2 -std=c++17 -o src/dxr_bloom src/dxr_bloom.cpp
./src/dxr_bloom
```
Outputs: `benchmarks/match_dxr_bloom.csv`, `benchmarks/results_dxr_bloom.csv`

## 4. Dynamic Operation Analysis

### Operation Costs (Radix Trie)
**File:** `src/ops_radix_trie.c`  
Measures individual operation costs (insert, delete, lookup) under batch and streaming modes.
```bash
gcc -O2 -o src/ops_radix_test src/ops_radix_trie.c
./src/ops_radix_test
```
Outputs: `benchmarks/ops_results_radix.csv`

### Mixed Read/Write Workloads
**Files:** `src/sim_radix_c.c`, `src/sim_dir_24_8.cpp`  
Simulates mixed workloads with varying read-to-write ratios to identify sustainable load thresholds.
```bash
# Radix trie simulation
gcc -O2 -o src/sim_radix src/sim_radix_c.c
./src/sim_radix

# DIR-24-8 simulation
g++ -O2 -std=c++17 -o src/sim_dir24_8 src/sim_dir_24_8.cpp
./src/sim_dir24_8
```
Outputs: `benchmarks/sim_radix.csv`, `benchmarks/sim_dir24_8.csv`

## 5. Verification and Plotting

### Correctness Verification
**File:** `benchmarks/verify_matches.py`  
Verifies that algorithm outputs match expected results from the generated IP table.
```bash
python3 benchmarks/verify_matches.py
```

### Automated Testing
**File:** `test_correctness.sh`  
Runs all algorithms, verifies correctness, and collects results.
```bash
./test_correctness.sh
```

### Plotting
**Directory:** `plots/`  
Python scripts generate performance plots from benchmark CSVs:
- `sim_radix_plot.py` - Mixed workload performance with plateau detection
- `ops_radix_plot.py` - Operation cost breakdowns
- Other plotting scripts for throughput, latency, and memory usage

## Example Workflow
```bash
# 1. Generate 10k random prefixes
./src/prefix_gen 10000

# 2. Generate 1M IP addresses from those prefixes
./src/ip_gen 1000000

# 3. Run all algorithms and verify correctness
./test_correctness.sh

# 4. Generate plots
cd plots && python3 sim_radix_plot.py
```

## Benchmark Results

Each algorithm generates two output files:
- **Match file** (`match_*.csv`): Maps each IP address to its matched key/prefix
- **Results file** (`results_*.csv`): Performance metrics including:
  - `prefix_load_s`, `build_ds_s`, `ip_load_s`, `lookup_s` — timing for each stage
  - `lookups_per_s`, `ns_per_lookup` — throughput in lookups/sec and nanoseconds per lookup
  - `mem_prefix_array_mb`, `mem_ds_mb`, `mem_ip_array_mb`, `mem_total_mb` — memory usage in MB

### Operation Cost Results
- `ops_results_radix.csv`: Per-operation costs (insert, delete, lookup) in nanoseconds
- `sim_radix.csv`, `sim_dir24_8.csv`: Mixed workload performance across read-to-write ratios

## Performance Metrics

The project measures:
- **Lookup latency** (nanoseconds per operation)
- **Throughput** (lookups per second)
- **Memory usage** (MB for data structures)
- **Build time** (seconds to construct the lookup structure)
- **Dynamic update costs** (insert/delete operation latency)
- **Sustainable load** (read-to-write ratio where writes have minimal impact)

## Research Context

This project is part of a research effort to evaluate IP lookup algorithms for packet source authentication systems. The algorithms are benchmarked under both static and dynamic workloads, with a focus on realistic routing scenarios where lookups dominate updates.