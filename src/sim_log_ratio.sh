#!/bin/bash
set -e

cd ..

# Compile
gcc -O2 -o src/res.o src/sim_radix_c.c

# Clear old results (optional: comment if you want to append instead)
#rm -f benchmarks/sim_radix.csv

# Generate log-spaced ratios from 1 to 10000 (about 40 samples)
ratios=$(python3 - <<EOF
import numpy as np
vals = np.unique(np.logspace(0, np.log10(10000000), num=40, dtype=int))
print(" ".join(map(str, vals)))
EOF
)

# Run the program for each ratio
for r in $ratios; do
    echo "Running ratio 1:$r"
    ./src/res.o $r
done

echo "All simulations completed. Results saved in benchmarks/sim_radix.csv"
