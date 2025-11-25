#!/bin/bash
set -euo pipefail

cd ..

# Compile DIR-24-8 simulator
g++ -O2 -o src/res_dir.o src/sim_dir_24_8.cpp

# Clear old results (optional: comment out if you want to append)
#rm -f benchmarks/sim_dir24_8.csv

# Generate log-spaced ratios from 1 to 10,000,000 (about 40 samples)
ratios=$(python3 - <<EOF
import numpy as np
vals = np.unique(np.logspace(0, np.log10(10000000), num=40, dtype=int))
print(" ".join(map(str, vals)))
EOF
)

# Run the program for each ratio
for r in $ratios; do
    echo "Running ratio 1:$r"
    ./src/res_dir.o $r
done

echo "All simulations completed. Results saved in benchmarks/sim_dir24_8.csv"
