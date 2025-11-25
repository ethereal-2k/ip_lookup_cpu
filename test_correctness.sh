#!/bin/bash
# Test correctness of all algorithms using verify_matches.py

set -e

cd "$(dirname "$0")"

echo "=== Testing Algorithm Correctness ==="
echo ""

# Check if data files exist
if [ ! -f "data/prefix_table.csv" ] || [ ! -f "data/generated_ips.csv" ]; then
    echo "ERROR: Data files not found. Please generate them first."
    exit 1
fi

# List of algorithms and their match files
declare -A algorithms=(
    ["binary_radix_trie"]="benchmarks/match_radix.csv"
    ["patricia_trie"]="benchmarks/match_pat.csv"
    ["dir24_8"]="benchmarks/match_dir24_8.csv"
    ["dxr"]="benchmarks/match_dxr.csv"
    ["dxr_bloom"]="benchmarks/match_dxr_bloom.csv"
    ["radix_trie_C"]="benchmarks/match_radix_C.csv"
)

# Check if binaries exist and are recent
echo "Checking binaries..."
for algo in "${!algorithms[@]}"; do
    case $algo in
        binary_radix_trie)
            bin="src/radix.out"
            ;;
        patricia_trie)
            bin="src/patricia.out"
            ;;
        dir24_8)
            bin="src/dir24_8.out"
            ;;
        dxr)
            bin="src/dxr.out"
            ;;
        dxr_bloom)
            bin="src/dxr_bloom.out"
            ;;
        radix_trie_C)
            bin="src/radix_trie_C.out"
            if [ ! -f "$bin" ]; then
                bin="src/radix.out"  # fallback
            fi
            ;;
    esac
    
    if [ ! -f "$bin" ]; then
        echo "WARNING: Binary not found for $algo: $bin"
        echo "  Please compile: g++ -O2 -std=c++17 -o $bin src/${algo}.cpp"
        continue
    fi
    
    echo "  Found: $bin"
done

echo ""
echo "Running algorithms to generate match files..."
echo ""

# Run each algorithm
for algo in "${!algorithms[@]}"; do
    match_file="${algorithms[$algo]}"
    echo "--- Testing $algo ---"
    
    case $algo in
        binary_radix_trie)
            bin="src/radix.out"
            if [ -f "$bin" ]; then
                echo "  Running $bin -chk..."
                "$bin" -chk > /dev/null 2>&1 || echo "  ERROR: Execution failed"
            else
                echo "  SKIP: Binary not found"
                continue
            fi
            ;;
        patricia_trie)
            bin="src/patricia.out"
            if [ -f "$bin" ]; then
                echo "  Running $bin -chk..."
                "$bin" -chk > /dev/null 2>&1 || echo "  ERROR: Execution failed"
            else
                echo "  SKIP: Binary not found"
                continue
            fi
            ;;
        dir24_8)
            bin="src/dir24_8.out"
            if [ -f "$bin" ]; then
                echo "  Running $bin -chk..."
                "$bin" -chk > /dev/null 2>&1 || echo "  ERROR: Execution failed"
            else
                echo "  SKIP: Binary not found"
                continue
            fi
            ;;
        dxr)
            bin="src/dxr.out"
            if [ -f "$bin" ]; then
                echo "  Running $bin -chk..."
                "$bin" -chk > /dev/null 2>&1 || echo "  ERROR: Execution failed"
            else
                echo "  SKIP: Binary not found"
                continue
            fi
            ;;
        dxr_bloom)
            bin="src/dxr_bloom.out"
            if [ -f "$bin" ]; then
                echo "  Running $bin -chk..."
                "$bin" -chk > /dev/null 2>&1 || echo "  ERROR: Execution failed"
            else
                echo "  SKIP: Binary not found"
                continue
            fi
            ;;
        radix_trie_C)
            bin="src/radix_trie_C.out"
            if [ ! -f "$bin" ]; then
                bin="src/radix.out"
            fi
            if [ -f "$bin" ]; then
                echo "  Running $bin -chk..."
                "$bin" -chk > /dev/null 2>&1 || echo "  ERROR: Execution failed"
            else
                echo "  SKIP: Binary not found"
                continue
            fi
            ;;
    esac
    
    # Verify results
    if [ -f "$match_file" ]; then
        echo "  Verifying $match_file..."
        python3 benchmarks/verify_matches.py --match "$match_file" --mismatches "benchmarks/mismatches_${algo}.csv" 2>&1 | tail -10
        echo ""
    else
        echo "  ERROR: Match file not generated: $match_file"
        echo ""
    fi
done

echo "=== Correctness Testing Complete ==="

