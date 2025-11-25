import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import os

# Paths
csv_path = os.path.join("..", "benchmarks", "sim_radix.csv")
out_path = "sim_radix_plot.png"

# Load CSV
df = pd.read_csv(csv_path)

# Extract numeric ratio (the read side of "1:n")
df['ratio_value'] = df['write_per_read_ratio'].apply(lambda x: int(x.split(":")[1]))

# Sort by ratio for nicer plotting
df = df.sort_values('ratio_value')

# Detect plateau: find where the curve flattens (slope becomes consistently small)
# Method: Calculate relative change and find where it stabilizes

# Calculate relative change (percentage change) between consecutive points
df['pct_change'] = (df['avg_total_ns'].diff().abs() / df['avg_total_ns'].shift(1).abs()) * 100
df['pct_change'] = df['pct_change'].fillna(0)

# Use rolling window to smooth out noise
window_size = min(4, len(df) // 3)
if window_size > 1:
    df['rolling_pct_change'] = df['pct_change'].rolling(window=window_size, center=True).mean()
    
    # Threshold: consider plateau when relative change is consistently < 3%
    threshold_pct = 3.0
    
    # Find first point where percentage change stays consistently small
    # Start from middle and work forward (plateau usually in second half)
    plateau_start = None
    search_start = max(window_size, len(df) // 3)
    
    for i in range(search_start, len(df) - window_size):
        # Check if next window_size points all have small relative changes
        window_changes = df.iloc[i:i+window_size]['rolling_pct_change']
        if window_changes.max() < threshold_pct and window_changes.mean() < threshold_pct * 0.7:
            plateau_start = i
            break
    
    if plateau_start is None:
        # Fallback: use last 40% of data where values are most stable
        plateau_start = int(len(df) * 0.6)
    
    # Use the plateau region (from plateau_start to end)
    plateau_region = df.iloc[plateau_start:]
    flat_ratio = plateau_region.iloc[0]['ratio_value']
    flat_avg_ns = plateau_region['avg_total_ns'].mean()
    
    # Calculate std dev of plateau to show stability
    flat_std = plateau_region['avg_total_ns'].std()
else:
    # Not enough data, use last value
    flat_ratio = df.iloc[-1]['ratio_value']
    flat_avg_ns = df.iloc[-1]['avg_total_ns']
    flat_std = 0

# Plot
plt.figure(figsize=(10, 6))
plt.plot(df['ratio_value'], df['avg_total_ns'], marker='o', label='Average ns/op', linewidth=2)

# Add horizontal dotted line at flattened value
plt.axhline(y=flat_avg_ns, color='r', linestyle='--', linewidth=1.5, 
           label=f'Sustainable load (~{flat_avg_ns:.1f} ± {flat_std:.1f} ns/op)')

# Add vertical line at flattening ratio
plt.axvline(x=flat_ratio, color='r', linestyle='--', linewidth=1.5, alpha=0.5)

# Annotate the flattening point
plt.annotate(f'Flattens at 1:{flat_ratio}\n(~{flat_avg_ns:.1f} ns/op)',
            xy=(flat_ratio, flat_avg_ns),
            xytext=(flat_ratio * 0.3, flat_avg_ns * 1.5),
            arrowprops=dict(arrowstyle='->', color='red', alpha=0.7),
            fontsize=9,
            bbox=dict(boxstyle='round,pad=0.5', facecolor='yellow', alpha=0.3))

plt.xlabel("Read per Write (n in 1:n)", fontsize=11)
plt.ylabel("Average Time per Operation (ns)", fontsize=11)
plt.title("Average ns/op vs Write:Read Ratio\n(Sustainable Load Threshold)", fontsize=12)
plt.grid(True, alpha=0.3)
plt.legend(loc='best')

# Use log scale if ratios vary widely
if df['ratio_value'].max() / df['ratio_value'].min() > 50:
    plt.xscale("log")

plt.tight_layout()
plt.savefig(out_path, dpi=150)
plt.close()

print(f"Plot saved to {out_path}")
print(f"Flattening detected at ratio 1:{flat_ratio}, sustainable load: ~{flat_avg_ns:.1f} ns/op")