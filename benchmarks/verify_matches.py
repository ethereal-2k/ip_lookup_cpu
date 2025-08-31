#!/usr/bin/env python3
import csv
import argparse
from pathlib import Path

# ---------------- helpers: CIDR parsing & subnet check ----------------

def parse_cidr(cidr: str):
    """
    "A.B.C.D/len" -> (network_uint32, len)
    """
    ip_str, plen_str = cidr.strip().split("/")
    plen = int(plen_str)
    a, b, c, d = (int(x) for x in ip_str.split("."))
    ip = (a << 24) | (b << 16) | (c << 8) | d
    mask = 0 if plen == 0 else ((0xFFFFFFFF << (32 - plen)) & 0xFFFFFFFF)
    net = ip & mask
    return net, plen

def is_subnet(sub: str, sup: str) -> bool:
    """
    True if `sub` is the same or a more specific subnet of `sup`
    (i.e., sub ⊆ sup in CIDR terms).
    """
    net_sub, plen_sub = parse_cidr(sub)
    net_sup, plen_sup = parse_cidr(sup)
    if plen_sub < plen_sup:
        return False
    mask_sup = 0 if plen_sup == 0 else ((0xFFFFFFFF << (32 - plen_sup)) & 0xFFFFFFFF)
    return (net_sub & mask_sup) == net_sup

# ---------------- loaders ----------------

def load_key_to_prefix(prefix_csv: Path):
    """
    Build mapping: key(hex) -> prefix(CIDR).
    Warn if a key maps to multiple prefixes (shouldn't happen for your generator).
    """
    key2pref = {}
    with prefix_csv.open(newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            pref = (row.get("prefix") or "").strip()
            key  = (row.get("key") or "").strip()
            if not pref or not key:
                continue
            if key in key2pref and key2pref[key] != pref:
                print(f"[WARN] Key maps to multiple prefixes: {key2pref[key]} vs {pref} (key={key})")
            key2pref.setdefault(key, pref)
    return key2pref

def load_ip_to_expected_prefix(ips_csv: Path):
    """
    Build mapping: ip(str) -> expected_prefix(from generator).
    """
    ip2pref = {}
    with ips_csv.open(newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            ip = (row.get("ip") or "").strip()
            pref = (row.get("used_prefix") or "").strip()
            if ip:
                ip2pref[ip] = pref
    return ip2pref

# ---------------- verifier ----------------

def verify_matches(match_csv: Path, key2pref, ip2pref, mismatches_out: Path | None):
    total = 0
    ok_exact = 0
    ok_more_specific = 0
    missing_ip = 0
    missing_key = 0
    mismatches = []

    all_prefixes = set(key2pref.values())  # to ensure matched prefixes exist

    with match_csv.open(newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            ip = (row.get("ip") or "").strip()
            key = (row.get("key") or "").strip()
            total += 1

            # treat "-1" / empty as no-match
            if not key or key == "-1":
                missing_key += 1
                mismatches.append((ip, ip2pref.get(ip, "<UNKNOWN_IP>"), "<no-match>"))
                continue

            exp_pref = ip2pref.get(ip)
            if exp_pref is None or not exp_pref:
                missing_ip += 1
                mismatches.append((ip, "<UNKNOWN_IP>", f"<key:{key}>"))
                continue

            matched_pref = key2pref.get(key)
            if matched_pref is None or matched_pref not in all_prefixes:
                missing_key += 1
                mismatches.append((ip, exp_pref, f"<key-not-in-prefix-table:{key}>"))
                continue

            if matched_pref == exp_pref:
                ok_exact += 1
            elif is_subnet(matched_pref, exp_pref):
                ok_more_specific += 1
            else:
                mismatches.append((ip, exp_pref, matched_pref))

    # write mismatches (if any)
    if mismatches_out:
        mismatches_out.parent.mkdir(parents=True, exist_ok=True)
        with mismatches_out.open("w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["ip", "expected_prefix", "matched_prefix_from_key"])
            for row in mismatches:
                w.writerow(row)

    correct = ok_exact + ok_more_specific
    return {
        "total_rows_in_match": total,
        "correct": correct,
        "correct_exact": ok_exact,
        "correct_more_specific": ok_more_specific,
        "incorrect": len(mismatches),
        "missing_ip": missing_ip,
        "missing_key_or_no_match": missing_key,
    }

# ---------------- CLI ----------------

def main():
    ap = argparse.ArgumentParser(description="Verify match CSV against prefix table and generated IPs, accepting exact or more-specific matches.")
    ap.add_argument("--prefix", default="data/prefix_table.csv", help="Path to prefix_table.csv")
    ap.add_argument("--ips", default="data/generated_ips.csv", help="Path to generated_ips.csv")
    ap.add_argument("--match", required=True, help="Path to match file (e.g., benchmarks/match_dir24_8.csv)")
    ap.add_argument("--mismatches", default="benchmarks/mismatches.csv", help="Where to write mismatches CSV")
    args = ap.parse_args()

    prefix_path = Path(args.prefix)
    ips_path = Path(args.ips)
    match_path = Path(args.match)
    mismatches_path = Path(args.mismatches) if args.mismatches else None

    if not prefix_path.exists():
        print(f"ERROR: prefix file not found: {prefix_path}")
        return 1
    if not ips_path.exists():
        print(f"ERROR: ips file not found: {ips_path}")
        return 1
    if not match_path.exists():
        print(f"ERROR: match file not found: {match_path}")
        return 1

    key2pref = load_key_to_prefix(prefix_path)
    ip2pref  = load_ip_to_expected_prefix(ips_path)

    stats = verify_matches(match_path, key2pref, ip2pref, mismatches_out=mismatches_path)

    print("\n=== Verification Summary ===")
    for k, v in stats.items():
        print(f"{k}: {v}")
    acc = (stats["correct"] / stats["total_rows_in_match"]) * 100 if stats["total_rows_in_match"] else 0.0
    print(f"accuracy_percent: {acc:.4f}%")
    if mismatches_path:
        print(f"mismatches written to: {mismatches_path}")

    return 0

if __name__ == "__main__":
    raise SystemExit(main())
