#!/usr/bin/env python3
import os
import subprocess
import pandas as pd
import matplotlib.pyplot as plt

SRC_DIR = "src"
BENCH_DIR = "benchmarks"
PLOT_DIR = "plots"
DATA_DIR = "data"

# Algorithms and results files
algorithms = {
    "radix": "binary_radix_trie.cpp",
    "patricia": "patricia_trie.cpp",
    "dir24_8": "dir_24_8.cpp",
    "dxr": "dxr.cpp",
    "dxr_bloom": "dxr_bloom.cpp",
}
results_files = {
    "radix": os.path.join(BENCH_DIR, "results_radix.csv"),
    "patricia": os.path.join(BENCH_DIR, "results_pat.csv"),
    "dir24_8": os.path.join(BENCH_DIR, "results_dir24_8.csv"),
    "dxr": os.path.join(BENCH_DIR, "results_dxr.csv"),
    "dxr_bloom": os.path.join(BENCH_DIR, "results_dxr_bloom.csv"),
}

def run_cmd(cmd, cwd=None):
    print("▶", " ".join(cmd))
    subprocess.run(cmd, check=True, cwd=cwd)

def compile_algorithms():
    for name, cpp_file in algorithms.items():
        src_path = os.path.join(SRC_DIR, cpp_file)
        bin_path = os.path.join(SRC_DIR, f"{name}.out")
        run_cmd(["g++", "-O2", "-std=c++17", "-o", bin_path, src_path])

    # also ip_gen and prefix_gen
    run_cmd(["g++", "-O2", "-std=c++17", "-o", os.path.join(SRC_DIR, "ip_gen.out"),
             os.path.join(SRC_DIR, "ip_gen.cpp")])
    run_cmd(["g++", "-O2", "-std=c++17", "-o", os.path.join(SRC_DIR, "prefix_gen.out"),
             os.path.join(SRC_DIR, "prefix_gen.cpp")])

def regenerate_prefixes(n=10000):
    run_cmd(["./prefix_gen.out", str(n)], cwd=SRC_DIR)

def regenerate_ips(n_ips):
    run_cmd(["./ip_gen.out", str(n_ips)], cwd=SRC_DIR)

def run_algorithms():
    for name in algorithms.keys():
        bin_path = os.path.join(SRC_DIR, f"{name}.out")
        print(f"▶ Running {name}...")
        run_cmd([bin_path])

def collect_results(n_ips):
    dfs = []
    for name, path in results_files.items():
        if os.path.exists(path):
            df = pd.read_csv(path)
            df["algorithm"] = name
            df["num_ips"] = n_ips
            dfs.append(df)
    return pd.concat(dfs, ignore_index=True) if dfs else pd.DataFrame()

def plot(df):
    os.makedirs(PLOT_DIR, exist_ok=True)
    metrics = [c for c in df.columns if c not in ["algorithm","prefix_file","ip_file","num_ips"]]
    for metric in metrics:
        plt.figure(figsize=(10,6))
        pivot = df.pivot(index="num_ips", columns="algorithm", values=metric)
        pivot.plot(kind="bar", ax=plt.gca())
        plt.title(f"{metric} comparison")
        plt.ylabel(metric)
        plt.xlabel("Number of IPs")
        plt.legend(title="Algorithm")
        plt.tight_layout()
        out_path = os.path.join(PLOT_DIR, f"{metric}.png")
        plt.savefig(out_path)
        plt.close()
        print(f"📊 Saved {out_path}")

def main():
    compile_algorithms()
    regenerate_prefixes()
    all_dfs = []
    for n in range(1_000_000, 10_000_001, 1_000_000):
        regenerate_ips(n)
        run_algorithms()
        df = collect_results(n)
        if not df.empty:
            all_dfs.append(df)
    if all_dfs:
        final = pd.concat(all_dfs, ignore_index=True)
        final.to_csv(os.path.join(BENCH_DIR, "all_results.csv"), index=False)
        print("✅ All results saved to benchmarks/all_results.csv")
        plot(final)
    else:
        print("⚠️ No results collected!")

if __name__ == "__main__":
    main()
