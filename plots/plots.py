import os
import pandas as pd
import matplotlib.pyplot as plt

CSV_PATH = "benchmarks/all_results.csv"
PLOT_DIR = "plots"

def plot_all():
    os.makedirs(PLOT_DIR, exist_ok=True)

    df = pd.read_csv(CSV_PATH)

    # 🚩 Drop duplicates on (algorithm, num_ips)
    df = df.drop_duplicates(subset=["algorithm", "num_ips"], keep="last")

    # Pick numeric metrics
    metrics = [c for c in df.columns if c not in [
        "algorithm", "prefix_file", "ip_file", "num_prefixes"
    ]]

    for metric in metrics:
        pivot = df.pivot(index="num_ips", columns="algorithm", values=metric)

        plt.figure(figsize=(10, 6))
        pivot.plot(kind="bar", ax=plt.gca())
        plt.title(metric)
        plt.xlabel("Number of IPs")
        plt.ylabel(metric)
        plt.xticks(rotation=45)
        plt.tight_layout()
        out_path = os.path.join(PLOT_DIR, f"{metric}.png")
        plt.savefig(out_path)
        plt.close()
        print(f"📊 Saved {out_path}")

if __name__ == "__main__":
    plot_all()
