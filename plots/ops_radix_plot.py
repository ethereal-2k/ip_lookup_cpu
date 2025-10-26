import pandas as pd
import matplotlib.pyplot as plt
import os

# Paths (script is in plots/, CSV is in benchmarks/)
csv_path = os.path.join("..", "benchmarks", "ops_results_radix.csv")

# Load CSV
df = pd.read_csv(csv_path)

# Filter for our algorithm
df = df[df['algorithm'] == 'BinaryRadixTrie_C']

# ------------------- Bar Chart -------------------
plt.figure(figsize=(8, 6))
plt.bar(df['num_ops'] - 2000, df['insert_ns_per_op'], width=2000, label='Insert')
plt.bar(df['num_ops'], df['lookup_ns_per_op'], width=2000, label='Lookup')
plt.bar(df['num_ops'] + 2000, df['delete_ns_per_op'], width=2000, label='Delete')
plt.xlabel("Number of Ops (N)")
plt.ylabel("Nanoseconds per operation")
plt.title("Insert, Lookup, Delete Time per Operation")
plt.legend()
plt.savefig("ops_radix_bar.png")
plt.close()

# ------------------- Streaming Pie Chart -------------------
stream_ratios = [
    df['stream_ratio_insert'].mean(),
    df['stream_ratio_lookup'].mean(),
    df['stream_ratio_delete'].mean()
]

plt.figure(figsize=(6, 6))
plt.pie(stream_ratios,
        labels=['Insert', 'Lookup', 'Delete'],
        autopct='%1.1f%%',
        startangle=140)
plt.title("Streaming Ratios (Average)")
plt.savefig("ops_radix_streaming_pie.png")
plt.close()

# ------------------- Batch Pie Chart -------------------
batch_ratios = [
    df['batch_ratio_insert'].mean(),
    df['batch_ratio_lookup'].mean(),
    df['batch_ratio_delete'].mean()
]

plt.figure(figsize=(6, 6))
plt.pie(batch_ratios,
        labels=['Insert', 'Lookup', 'Delete'],
        autopct='%1.1f%%',
        startangle=140)
plt.title("Batch Ratios (Average)")
plt.savefig("ops_radix_batch_pie.png")
plt.close()
