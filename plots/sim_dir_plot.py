import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import os

# Paths (assuming script is in plots/, CSV in benchmarks/, output in plots/)
csv_file = os.path.join("..", "benchmarks", "sim_dir24_8.csv")
out_file = os.path.join(".", "sim_dir24_8_plot.png")

# Load data
df = pd.read_csv(csv_file)

# Parse ratio values (extract the number after "1:")
df["ratio_value"] = df["write_per_read_ratio"].str.split(":").str[1].astype(int)

# Sort by ratio
df = df.sort_values("ratio_value")

# Plot avg_total_ns vs ratio (log scale on x-axis for clarity)
plt.figure(figsize=(10,6))
plt.plot(df["ratio_value"], df["avg_total_ns"], marker="o", linestyle="-", label="Avg Total ns/op")

plt.xscale("log")
plt.xlabel("Read per Write Ratio (log scale)")
plt.ylabel("Average Time per Operation (ns)")
plt.title("DIR-24-8 Simulation: Average Time per Operation vs Read/Write Ratio")
plt.grid(True, which="both", linestyle="--", alpha=0.6)
plt.legend()

plt.tight_layout()
plt.savefig(out_file, dpi=300)
print(f"Plot saved to {out_file}")
