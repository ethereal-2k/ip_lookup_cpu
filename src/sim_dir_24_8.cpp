#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <iomanip>
#include <chrono>
#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <random>
#include <algorithm>

// ------------------------- Config / constants -------------------------
static const int MAIN_TABLE_SIZE = 1 << 24;  // 2^24
static const int SUBTABLE_SIZE   = 256;

// File paths (relative to repo root)
static const char* PREFIX_FILE   = "data/prefix_table.csv";
static const char* IP_FILE       = "data/generated_ips.csv";
static const char* SIM_FILE      = "benchmarks/sim_dir24_8.csv";

// ------------------------- Timing helpers -----------------------------
static inline uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

// ------------------------- Utilities ---------------------------------
static inline uint32_t mask_from_len(uint8_t len) {
    return (len == 0) ? 0U : (~0U << (32 - len));
}
static inline uint32_t ip_str_to_uint(const std::string& ip_str) {
    in_addr addr{};
    inet_pton(AF_INET, ip_str.c_str(), &addr);
    return ntohl(addr.s_addr);
}
static inline bool file_exists(const char* path) {
    std::ifstream f(path);
    return f.good();
}

// ------------------------- Key pool ----------------------------------
// Dedup 64B keys from CSV
static std::unordered_map<std::string, uint8_t*> g_key_pool;

static uint8_t* get_or_create_key_from_hex(const std::string& hex) {
    if (hex.size() != 128) return nullptr;
    auto it = g_key_pool.find(hex);
    if (it != g_key_pool.end()) return it->second;
    uint8_t* bytes = new uint8_t[64];
    for (size_t i = 0; i < 128; i += 2) {
        bytes[i/2] = static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16));
    }
    g_key_pool.emplace(hex, bytes);
    return bytes;
}
static uint8_t* new_random_key(std::mt19937& rng) {
    uint8_t* key = new uint8_t[64];
    for (int i = 0; i < 64; ++i) key[i] = static_cast<uint8_t>(rng() & 0xFF);
    return key;
}

// ------------------------- Tries for correctness ---------------------
struct TrieNode {
    TrieNode* c[2]{nullptr, nullptr};
    bool has{false};
    uint8_t* key{nullptr};
    uint8_t  plen{0};
};
struct BinaryTrie {
    TrieNode* root{nullptr};
    BinaryTrie() { root = new TrieNode(); }
    ~BinaryTrie() { destroy(root); }

    void destroy(TrieNode* n) {
        if (!n) return;
        destroy(n->c[0]); destroy(n->c[1]); delete n;
    }

    void insert(uint32_t base, uint8_t len, uint8_t* key) {
        TrieNode* n = root;
        for (int i = 0; i < len; ++i) {
            int b = (base >> (31 - i)) & 1;
            if (!n->c[b]) n->c[b] = new TrieNode();
            n = n->c[b];
        }
        n->has = true; n->key = key; n->plen = len;
    }

    // remove the exact prefix; prune empty nodes
    bool remove_rec(TrieNode* n, uint32_t base, uint8_t len, int depth) {
        if (!n) return true;
        if (depth == len) {
            n->has = false; n->key = nullptr; n->plen = 0;
        } else {
            int b = (base >> (31 - depth)) & 1;
            if (remove_rec(n->c[b], base, len, depth + 1)) {
                delete n->c[b]; n->c[b] = nullptr;
            }
        }
        if (n->has) return false;
        return (n->c[0] == nullptr && n->c[1] == nullptr);
    }
    void remove(uint32_t base, uint8_t len) { remove_rec(root, base, len, 0); }

    // Longest-prefix match
    // Returns best key and plen (0 if none)
    std::pair<uint8_t*, uint8_t> lpm(uint32_t ip) const {
        TrieNode* n = root;
        uint8_t* best_key = nullptr; uint8_t best_plen = 0;
        if (n->has) { best_key = n->key; best_plen = n->plen; }
        for (int i = 0; i < 32 && n; ++i) {
            int b = (ip >> (31 - i)) & 1;
            n = n->c[b];
            if (!n) break;
            if (n->has) { best_key = n->key; best_plen = n->plen; }
        }
        return {best_key, best_plen};
    }
};

// ------------------------- DIR-24-8 tables ---------------------------
struct Cell { uint8_t* key{nullptr}; uint8_t plen{0}; };
struct Bucket {
    Cell def;     // best <= /24
    Cell* sub;    // 256 cells for > /24 (allocated on demand)
    Bucket(): sub(nullptr) {}
};
static Bucket* g_buckets = nullptr;

// ------------------------- Build baseline from CSV -------------------
static void build_from_csv(BinaryTrie& trie24, BinaryTrie& trie32) {
    std::ifstream fib(PREFIX_FILE);
    std::string line;
    std::getline(fib, line); // skip header
    size_t loaded = 0;

    while (std::getline(fib, line)) {
        std::istringstream ss(line);
        std::string prefix_str, key_hex;
        if (!std::getline(ss, prefix_str, ',')) continue;
        if (!std::getline(ss, key_hex)) continue;
        auto slash = prefix_str.find('/');
        if (slash == std::string::npos) continue;

        std::string ip_part = prefix_str.substr(0, slash);
        uint8_t len = static_cast<uint8_t>(std::stoi(prefix_str.substr(slash + 1)));
        if (len > 32) continue;  // Skip invalid prefix lengths
        uint32_t base_ip = ip_str_to_uint(ip_part) & mask_from_len(len);
        uint8_t* key = get_or_create_key_from_hex(key_hex);

        if (len <= 24) {
            trie24.insert(base_ip, len, key);
            const uint32_t start = base_ip >> 8;
            const uint32_t fill  = 1u << (24 - len);
            for (uint32_t i = 0; i < fill; ++i) {
                Cell& d = g_buckets[start + i].def;
                if (d.plen <= len) { d.key = key; d.plen = len; }  // Use <= to allow exact prefix updates
            }
        } else {
            trie32.insert(base_ip, len, key);
            const uint32_t count = 1u << (32 - len);
            for (uint32_t off = 0; off < count; ++off) {
                uint32_t ip_full  = base_ip + off;
                uint32_t main_idx = ip_full >> 8;
                uint8_t  sub_idx  = static_cast<uint8_t>(ip_full & 0xFF);
                Bucket& b = g_buckets[main_idx];
                if (!b.sub) b.sub = new Cell[SUBTABLE_SIZE]();
                Cell& c = b.sub[sub_idx];
                if (c.plen <= len) { c.key = key; c.plen = len; }  // Use <= to allow exact prefix updates
            }
        }
        ++loaded;
    }
    std::cout << "Baseline loaded prefixes: " << loaded << "\n";
}

// ------------------------- Dynamic insert/delete ---------------------
// Insert: update trie then the tables (only where more specific)
static void dir_insert(BinaryTrie& trie24, BinaryTrie& trie32,
                       uint32_t base_ip, uint8_t len, uint8_t* key)
{
    if (len > 32) return;  // Validate prefix length
    if (len <= 24) {
        trie24.insert(base_ip, len, key);
        const uint32_t start = base_ip >> 8;
        const uint32_t fill  = 1u << (24 - len);
        for (uint32_t i = 0; i < fill; ++i) {
            Cell& d = g_buckets[start + i].def;
            if (d.plen <= len) { d.key = key; d.plen = len; }  // Use <= to allow exact prefix updates
        }
    } else {
        trie32.insert(base_ip, len, key);
        const uint32_t count = 1u << (32 - len);
        for (uint32_t off = 0; off < count; ++off) {
            uint32_t ip_full  = base_ip + off;
            uint32_t main_idx = ip_full >> 8;
            uint8_t  sub_idx  = static_cast<uint8_t>(ip_full & 0xFF);
            Bucket& b = g_buckets[main_idx];
            if (!b.sub) b.sub = new Cell[SUBTABLE_SIZE]();
            Cell& c = b.sub[sub_idx];
            if (c.plen <= len) { c.key = key; c.plen = len; }  // Use <= to allow exact prefix updates
        }
    }
}

// Delete: remove from trie, then recompute affected range from trie
static void dir_delete(BinaryTrie& trie24, BinaryTrie& trie32,
                       uint32_t base_ip, uint8_t len)
{
    if (len <= 24) {
        trie24.remove(base_ip, len);
        const uint32_t start = base_ip >> 8;
        const uint32_t fill  = 1u << (24 - len);
        for (uint32_t i = 0; i < fill; ++i) {
            uint32_t ip_rep = ((start + i) << 8); // any IP within this /24
            // Check trie32 first for more specific /25+ prefixes
            auto best32 = trie32.lpm(ip_rep);
            if (best32.second > 0) {
                Cell& d = g_buckets[start + i].def;
                d.key = best32.first; d.plen = best32.second;
            } else {
                // Fall back to trie24 for /24- prefixes
                auto best24 = trie24.lpm(ip_rep);
                Cell& d = g_buckets[start + i].def;
                d.key = best24.first; d.plen = best24.second; // 0 if none
            }
        }
    } else {
        trie32.remove(base_ip, len);
        const uint32_t count = 1u << (32 - len);
        for (uint32_t off = 0; off < count; ++off) {
            uint32_t ip_full  = base_ip + off;
            uint32_t main_idx = ip_full >> 8;
            uint8_t  sub_idx  = static_cast<uint8_t>(ip_full & 0xFF);
            // Check trie32 first for /25+ prefixes
            auto best32 = trie32.lpm(ip_full);
            Bucket& b = g_buckets[main_idx];
            if (!b.sub) b.sub = new Cell[SUBTABLE_SIZE](); // ensure exist
            Cell& c = b.sub[sub_idx];
            if (best32.second > 0) {
                c.key = best32.first; c.plen = best32.second;
            } else {
                // Fall back to trie24 for /24- default
                uint32_t ip_rep = (main_idx << 8);
                auto best24 = trie24.lpm(ip_rep);
                c.key = best24.first; c.plen = best24.second; // 0 if none
            }
        }
    }
}

// Lookup: check sub then default
static inline const uint8_t* dir_lookup(uint32_t ip) {
    uint32_t main_idx = ip >> 8;
    uint8_t  sub_idx  = static_cast<uint8_t>(ip & 0xFF);
    Bucket& b = g_buckets[main_idx];
    if (b.sub) {
        const Cell& c = b.sub[sub_idx];
        if (c.key && c.plen > 0) return c.key;  // Only return if plen > 0 (valid match)
    }
    return (b.def.plen > 0) ? b.def.key : nullptr;  // Only return if plen > 0
}

// ------------------------- Dynamic prefix generator ------------------
struct DynPrefix { uint32_t base; uint8_t len; uint8_t* key; };

static std::vector<DynPrefix> generate_dyn_prefixes(size_t pairs,
                                                    std::mt19937& rng,
                                                    int min_len = 8, int max_len = 32) {
    std::vector<DynPrefix> v;
    v.reserve(pairs);
    std::uniform_int_distribution<uint32_t> ip_dist(0, 0xFFFFFFFFu);
    std::uniform_int_distribution<int> len_dist(min_len, max_len);
    for (size_t i = 0; i < pairs; ++i) {
        uint8_t len = static_cast<uint8_t>(len_dist(rng));
        uint32_t ip = ip_dist(rng);
        uint32_t base = ip & mask_from_len(len);
        v.push_back({base, len, nullptr}); // key set when inserting
    }
    return v;
}

// ------------------------- Main (mixed workload) ---------------------
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <n lookups per write> [num_ops]\n";
        return 1;
    }
    int n = std::atoi(argv[1]); // 1 write per n lookups
    if (n <= 0) { std::cerr << "n must be > 0\n"; return 1; }

    size_t N = 1'000'000; // default total ops
    if (argc >= 3) {
        long long inN = std::atoll(argv[2]);
        if (inN > 0) N = (size_t)inN;
    }

    // Allocate DIR-24-8 buckets
    g_buckets = new Bucket[MAIN_TABLE_SIZE];

    // Tries for correctness
    BinaryTrie trie24; // /0..24
    BinaryTrie trie32; // /25..32

    // Build baseline from CSV
    if (!file_exists(PREFIX_FILE)) {
        std::cerr << "Error: cannot open " << PREFIX_FILE << "\n";
        return 1;
    }
    build_from_csv(trie24, trie32);

    // Load IPs for lookup
    if (!file_exists(IP_FILE)) {
        std::cerr << "Error: cannot open " << IP_FILE << "\n";
        return 1;
    }
    std::vector<uint32_t> ips;
    ips.reserve(1'000'000);
    {
        std::ifstream ipfile(IP_FILE);
        std::string line; std::getline(ipfile, line); // header
        while (std::getline(ipfile, line)) {
            std::istringstream ss(line);
            std::string ip_str, ignore;
            if (!std::getline(ss, ip_str, ',')) continue;
            std::getline(ss, ignore);
            ips.push_back(ip_str_to_uint(ip_str));
        }
    }
    if (ips.empty()) { std::cerr << "No IPs loaded\n"; return 1; }

    // Prepare lookup sequence (avoid modulo reuse bias)
    std::random_device rd; std::mt19937 rng(rd());
    std::uniform_int_distribution<size_t> ip_idx(0, ips.size() - 1);
    std::vector<uint32_t> lookup_seq; lookup_seq.reserve(N);
    for (size_t i = 0; i < N; ++i) lookup_seq.push_back(ips[ip_idx(rng)]);

    // Prepare dynamic write prefixes (pairs of insert/delete)
    size_t expected_writes = std::max<size_t>(1, N / (size_t)(n + 1));
    size_t pairs = (expected_writes + 1) / 2 + 8; // a bit extra
    auto dyn = generate_dyn_prefixes(pairs, rng, /*min_len=*/8, /*max_len=*/32);

    // Mixed workload
    uint64_t total_lookup_ns = 0, total_write_ns = 0;
    size_t num_lookups = 0, num_writes = 0;
    volatile size_t sink = 0;

    size_t write_pair_idx = 0; // index into dyn pairs (even->insert, odd->delete same pair)
    uint64_t t_all = now_ns();
    for (size_t i = 0; i < N; ++i) {
        if (i % (n + 1) == 0) {
            // ---- Write ---- alternate insert/delete on same pair
            uint64_t t0 = now_ns();
            if ((num_writes & 1) == 0) {
                // INSERT
                if (write_pair_idx >= dyn.size()) {
                    std::cerr << "Error: write_pair_idx overflow\n";
                    break;
                }
                DynPrefix& p = dyn[write_pair_idx];
                if (!p.key) p.key = new_random_key(rng);
                dir_insert(trie24, trie32, p.base, p.len, p.key);
            } else {
                // DELETE
                if (write_pair_idx >= dyn.size()) {
                    std::cerr << "Error: write_pair_idx overflow\n";
                    break;
                }
                DynPrefix& p = dyn[write_pair_idx];
                dir_delete(trie24, trie32, p.base, p.len);
                ++write_pair_idx; // advance pair only after delete
            }
            total_write_ns += (now_ns() - t0);
            ++num_writes;
        } else {
            // ---- Lookup ----
            uint32_t ip = lookup_seq[i];
            uint64_t t0 = now_ns();
            const uint8_t* k = dir_lookup(ip);
            sink ^= (k ? k[0] : 0);
            total_lookup_ns += (now_ns() - t0);
            ++num_lookups;
        }
    }
    uint64_t elapsed_ns = now_ns() - t_all;
    (void)sink;

    // Averages
    double avg_lookup_ns = (num_lookups ? (double)total_lookup_ns / num_lookups : 0.0);
    double avg_write_ns  = (num_writes  ? (double)total_write_ns  / num_writes  : 0.0);
    double avg_total_ns  = (double)elapsed_ns / N;

    std::cout << "Ratio 1:" << n
              << "  Lookups=" << num_lookups
              << "  Writes="  << num_writes << "\n";
    std::cout << std::fixed << std::setprecision(2)
              << "Avg lookup = " << avg_lookup_ns
              << " ns, Avg write = " << avg_write_ns
              << " ns, Overall = "   << avg_total_ns << " ns/op\n";

    // CSV
    bool need_header = !file_exists(SIM_FILE);
    std::ofstream out(SIM_FILE, std::ios::app);
    if (out) {
        if (need_header) {
            out << "write_per_read_ratio,num_ops,num_lookups,num_writes,"
                   "avg_lookup_ns,avg_write_ns,avg_total_ns\n";
        }
        out << "1:" << n << ","
            << N << ","
            << num_lookups << ","
            << num_writes << ","
            << std::fixed << std::setprecision(2)
            << avg_lookup_ns << ","
            << avg_write_ns  << ","
            << avg_total_ns  << "\n";
    } else {
        std::cerr << "Error: cannot open " << SIM_FILE << " for writing\n";
    }

    // Cleanup: free keys (from CSV pool)
    for (auto& kv : g_key_pool) delete[] kv.second;
    g_key_pool.clear();

    // Cleanup: free dynamic keys
    for (auto& p : dyn) { delete[] p.key; p.key = nullptr; }

    // Free DIR-24-8 tables
    if (g_buckets) {
        for (int i = 0; i < MAIN_TABLE_SIZE; ++i) {
            delete[] g_buckets[i].sub;
        }
        delete[] g_buckets;
        g_buckets = nullptr;
    }
    return 0;
}
