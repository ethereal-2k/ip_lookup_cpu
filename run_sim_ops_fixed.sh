#!/bin/bash
# Run sim_radix and ops_radix, parsing output to create CSV files
# (Workaround for binary file writing issues)

set -e

cd "$(dirname "$0")"

echo "=== Running Simulation and Ops Tests ==="
echo ""

# Initialize CSV files
echo "write_per_read_ratio,num_ops,num_lookups,num_writes,avg_lookup_ns,avg_write_ns,avg_total_ns" > benchmarks/sim_radix.csv
echo "algorithm,num_prefixes,num_ops,num_ips,insert_time,lookup_time,delete_time,mixed_time,insert_ops_per_s,lookup_ops_per_s,delete_ops_per_s,mixed_ops_per_s,insert_ns_per_op,lookup_ns_per_op,delete_ns_per_op,mixed_ns_per_op,batch_ratio_insert,batch_ratio_lookup,batch_ratio_delete,stream_ratio_insert,stream_ratio_lookup,stream_ratio_delete" > benchmarks/ops_results_radix.csv

# Find sim binary
SIM_BIN=""
for bin in src/res.o src/sim_radix_c.out; do
    if [ -f "$bin" ] && [ -x "$bin" ]; then
        SIM_BIN="$bin"
        break
    fi
done

# Find ops binary
OPS_BIN=""
for bin in src/ops_radix_test.out src/radix.out; do
    if [ -f "$bin" ] && [ -x "$bin" ]; then
        OPS_BIN="$bin"
        break
    fi
done

# Run sim_radix with various ratios
if [ -n "$SIM_BIN" ]; then
    echo "--- Running sim_radix tests ---"
    ratios=(1 2 5 10 20 50 100 200 500 1000 2000 5000 10000 20000 50000 100000 200000 500000 1000000)
    
    for ratio in "${ratios[@]}"; do
        echo "  Running ratio 1:$ratio..."
        output=$("$SIM_BIN" "$ratio" 2>&1)
        # Parse output: "Ratio 1:100  Lookups=990099  Writes=9901"
        # "Avg lookup = 145.79 ns, Avg write = 28345.14 ns, Overall = 437.52 ns/op"
        if echo "$output" | grep -q "Ratio"; then
            lookups=$(echo "$output" | grep "Lookups=" | sed 's/.*Lookups=\([0-9]*\).*/\1/')
            writes=$(echo "$output" | grep "Writes=" | sed 's/.*Writes=\([0-9]*\).*/\1/')
            lookup_ns=$(echo "$output" | grep "Avg lookup" | sed 's/.*Avg lookup = \([0-9.]*\) ns.*/\1/')
            write_ns=$(echo "$output" | grep "Avg write" | sed 's/.*Avg write = \([0-9.]*\) ns.*/\1/')
            total_ns=$(echo "$output" | grep "Overall" | sed 's/.*Overall = \([0-9.]*\) ns.*/\1/')
            num_ops=$((lookups + writes))
            echo "1:$ratio,$num_ops,$lookups,$writes,$lookup_ns,$write_ns,$total_ns" >> benchmarks/sim_radix.csv
        fi
    done
    echo "  sim_radix tests complete"
    wc -l benchmarks/sim_radix.csv
    echo ""
else
    echo "WARNING: sim_radix binary not found"
    echo ""
fi

# Run ops_radix
if [ -n "$OPS_BIN" ]; then
    echo "--- Running ops_radix tests ---"
    output=$("$OPS_BIN" 2>&1)
    # The output should contain timing info - we'll need to parse it
    # For now, just run it - the binary should write to CSV itself
    echo "$output" | grep -E "(Insert|Lookup|Delete|Batch|Streaming)" || true
    echo "  ops_radix tests complete"
    if [ -f "benchmarks/ops_results_radix.csv" ]; then
        wc -l benchmarks/ops_results_radix.csv
    fi
    echo ""
else
    echo "WARNING: ops_radix binary not found"
    echo ""
fi

# Generate plots
echo "--- Generating Plots ---"
cd plots

if [ -f "../benchmarks/sim_radix.csv" ] && [ $(wc -l < ../benchmarks/sim_radix.csv) -gt 1 ]; then
    echo "  Generating sim_radix plot..."
    python3 sim_radix_plot.py 2>&1 | grep -v "Bad key" | grep -v "virtual const" || true
    echo "  ✓ sim_radix plot generated"
else
    echo "  WARNING: benchmarks/sim_radix.csv not found or empty"
fi

if [ -f "../benchmarks/ops_results_radix.csv" ] && [ $(wc -l < ../benchmarks/ops_results_radix.csv) -gt 1 ]; then
    echo "  Generating ops_radix plots..."
    python3 ops_radix_plot.py 2>&1 | grep -v "Bad key" | grep -v "virtual const" || true
    echo "  ✓ ops_radix plots generated"
else
    echo "  WARNING: benchmarks/ops_results_radix.csv not found or empty"
fi

cd ..

echo ""
echo "=== Complete ==="

