#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include <chrono>
#include <arpa/inet.h>
#include <unistd.h>   // sysconf
#include <cstdint>
#include <algorithm>

/// Usage:
///   Fast mode (default):   ./src/radix_trie
///   Check mode (hex out):  ./src/radix_trie -chk

// ------------------------- Paths -------------------------
static const char* PREFIX_FILE   = "data/prefix_table.csv";
static const char* IP_FILE       = "data/generated_ips.csv";
static const char* MATCH_FILE    = "benchmarks/match_radix.csv";
static const char* RESULTS_FILE  = "benchmarks/results_radix.csv";

// ------------------------- Helpers -----------------------
static inline uint32_t ip_str_to_uint(const std::string& ip_str) {
    in_addr addr{};
    inet_pton(AF_INET, ip_str.c_str(), &addr);
    return ntohl(addr.s_addr);
}
static inline uint32_t mask_from_len(uint8_t len) {
    return (len == 0) ? 0U : (~0U << (32 - len));
}
static inline std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}
static inline std::string bytes_to_hex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    for (auto b : bytes)
        oss << std::hex << std::setw(2) << std::setfill('0') << int(b);
    return oss.str();
}
static inline bool file_exists(const char* path) {
    std::ifstream f(path);
    return f.good();
}
static inline auto now() { return std::chrono::high_resolution_clock::now(); }
static inline double secs_since(std::chrono::high_resolution_clock::time_point t0) {
    return std::chrono::duration<double>(now() - t0).count();
}
static inline size_t current_rss_bytes() {
    std::ifstream statm("/proc/self/statm");
    size_t size=0, resident=0;
    if (statm) statm >> size >> resident;
    return resident * static_cast<size_t>(sysconf(_SC_PAGESIZE));
}
static inline double bytes_to_mb(size_t b) { return double(b) / (1024.0 * 1024.0); }

// ------------------------- Data --------------------------
struct Node {
    Node* child[2]{nullptr, nullptr};
    bool has_key = false;
    std::vector<uint8_t> key; // key only at prefix end
};
class BinaryTrie {
public:
    BinaryTrie() : root(new Node) {}
    ~BinaryTrie() { destroy(root); }

    // Insert taking ownership of key via move
    void insert_move(uint32_t net, uint8_t len, std::vector<uint8_t>&& key) {
        if (len > 0) net &= mask_from_len(len);
        Node* n = root;
        for (int i = 0; i < len; ++i) {
            int bit = (net >> (31 - i)) & 1;
            if (!n->child[bit]) n->child[bit] = new Node();
            n = n->child[bit];
        }
        n->has_key = true;
        n->key = std::move(key);
        ++inserted_;
    }

    const std::vector<uint8_t>* lpm(uint32_t ip) const {
        const std::vector<uint8_t>* best = nullptr;
        const Node* n = root;
        if (n->has_key) best = &n->key;
        for (int i = 0; i < 32 && n; ++i) {
            int bit = (ip >> (31 - i)) & 1;
            n = n->child[bit];
            if (!n) break;
            if (n->has_key) best = &n->key;
        }
        return best;
    }

    size_t inserted() const { return inserted_; }

private:
    Node* root;
    size_t inserted_ = 0;
    void destroy(Node* n) {
        if (!n) return;
        destroy(n->child[0]);
        destroy(n->child[1]);
        delete n;
    }
};

struct PrefixRec {
    uint32_t net;
    uint8_t len;
    std::vector<uint8_t> key; // will be moved into trie
};

// ------------------------- Main --------------------------
int main(int argc, char* argv[]) {
    // Simple flag: -chk -> output real hex keys for correctness checking
    bool write_hex = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-chk" || a == "--chk") write_hex = true;
        else if (a == "-h" || a == "--help") {
            std::cout <<
                "Usage: " << argv[0] << " [-chk]\n"
                "  -chk   Write hex keys to benchmarks/match_radix.csv (slower)\n";
            return 0;
        }
    }

    // Baseline memory
    size_t rss_baseline = current_rss_bytes();

    // -------- Phase A: Load prefixes (batch) --------
    if (!file_exists(PREFIX_FILE)) {
        std::cerr << "Error: cannot open " << PREFIX_FILE << "\n";
        return 1;
    }
    auto tA0 = now();
    size_t rssA0 = current_rss_bytes();

    std::ifstream pf(PREFIX_FILE);
    std::string line;
    std::getline(pf, line); // header: prefix,key

    std::vector<PrefixRec> prefixes;
    prefixes.reserve(200000);

    size_t num_prefixes = 0, bad_rows = 0;
    while (std::getline(pf, line)) {
        std::istringstream ss(line);
        std::string prefix_str, key_hex;
        if (!std::getline(ss, prefix_str, ',') || !std::getline(ss, key_hex)) { ++bad_rows; continue; }
        auto slash = prefix_str.find('/');
        if (slash == std::string::npos) { ++bad_rows; continue; }

        std::string ip_part = prefix_str.substr(0, slash);
        uint8_t len = static_cast<uint8_t>(std::stoi(prefix_str.substr(slash + 1)));
        uint32_t net = ip_str_to_uint(ip_part) & mask_from_len(len);
        std::vector<uint8_t> key = hex_to_bytes(key_hex);

        prefixes.push_back({net, len, std::move(key)});
        ++num_prefixes;
    }

    double prefix_load_s = secs_since(tA0);
    size_t rssA1 = current_rss_bytes();
    size_t mem_prefix_array_bytes = (rssA1 > rssA0 ? rssA1 - rssA0 : 0);

    // -------- Phase B: Build trie (move keys into nodes) --------
    auto tB0 = now();
    size_t rssB0 = current_rss_bytes();

    BinaryTrie trie;
    for (auto& rec : prefixes) {
        trie.insert_move(rec.net, rec.len, std::move(rec.key));
    }
    double build_ds_s = secs_since(tB0);
    size_t rssB1 = current_rss_bytes();
    size_t mem_ds_bytes = (rssB1 > rssB0 ? rssB1 - rssB0 : 0);

    // free prefix array memory now that keys have moved to the trie
    prefixes.clear();
    prefixes.shrink_to_fit();

    // -------- Phase C: Load IPs (batch) --------
    if (!file_exists(IP_FILE)) {
        std::cerr << "Error: cannot open " << IP_FILE << "\n";
        return 1;
    }
    auto tC0 = now();
    size_t rssC0 = current_rss_bytes();

    std::ifstream ipf(IP_FILE);
    std::getline(ipf, line); // header: ip,used_prefix

    std::vector<uint32_t> ips;
    std::vector<std::string> ip_strs;
    ips.reserve(1<<20);
    ip_strs.reserve(1<<20);

    while (std::getline(ipf, line)) {
        std::istringstream ss(line);
        std::string ip_str, dummy;
        if (!std::getline(ss, ip_str, ',')) continue;
        std::getline(ss, dummy);
        ip_strs.push_back(ip_str);
        ips.push_back(ip_str_to_uint(ip_str));
    }

    double ip_load_s = secs_since(tC0);
    size_t rssC1 = current_rss_bytes();
    size_t mem_ip_array_bytes = (rssC1 > rssC0 ? rssC1 - rssC0 : 0);

    // -------- Phase D: Lookup timing --------
    auto tD0 = now();
    std::vector<std::pair<std::string,std::string>> results;
    results.reserve(ips.size());

    for (size_t i = 0; i < ips.size(); ++i) {
        const auto* key = trie.lpm(ips[i]);
        if (write_hex) {
            results.emplace_back(ip_strs[i], key ? bytes_to_hex(*key) : std::string("-1"));
        } else {
            results.emplace_back(ip_strs[i], key ? std::string("1") : std::string("-1"));
        }
    }
    double lookup_s = secs_since(tD0);

    double ns_per_lookup = ips.empty() ? 0.0 : (lookup_s * 1e9 / double(ips.size()));
    double lookups_per_s = (lookup_s > 0.0) ? (double(ips.size()) / lookup_s) : 0.0;

    // -------- Write matches --------
    {
        std::ofstream out(MATCH_FILE, std::ios::binary);
        if (!out) {
            std::cerr << "Error: cannot open " << MATCH_FILE << " for writing\n";
        } else {
            out << "ip,key\n";
            for (auto& r : results) out << r.first << "," << r.second << "\n";
        }
    }

    // -------- Final mem & write results CSV --------
    size_t rss_total_bytes = current_rss_bytes();
    double mem_prefix_array_mb = bytes_to_mb(mem_prefix_array_bytes);
    double mem_ds_mb           = bytes_to_mb(mem_ds_bytes);
    double mem_ip_array_mb     = bytes_to_mb(mem_ip_array_bytes);
    double mem_total_mb        = bytes_to_mb(rss_total_bytes);

    const char* algo_name = "BinaryRadixTrie";
    bool need_header = !file_exists(RESULTS_FILE);

    std::ofstream res(RESULTS_FILE, std::ios::app);
    if (!res) {
        std::cerr << "Error: cannot open " << RESULTS_FILE << " for writing\n";
    } else {
        res.setf(std::ios::fixed);
        if (need_header) {
            res << "algorithm,prefix_file,ip_file,num_prefixes,num_ips,"
                   "prefix_load_s,build_ds_s,ip_load_s,lookup_s,"
                   "lookups_per_s,ns_per_lookup,"
                   "mem_prefix_array_mb,mem_ds_mb,mem_ip_array_mb,mem_total_mb\n";
        }
        res << algo_name << ","
            << PREFIX_FILE << ","
            << IP_FILE << ","
            << num_prefixes << ","
            << ips.size() << ","
            << std::setprecision(6)
            << prefix_load_s << ","
            << build_ds_s << ","
            << ip_load_s << ","
            << lookup_s << ","
            << std::setprecision(2)
            << lookups_per_s << ","
            << ns_per_lookup << ","
            << std::setprecision(2)
            << mem_prefix_array_mb << ","
            << mem_ds_mb << ","
            << mem_ip_array_mb << ","
            << mem_total_mb
            << "\n";
    }

    return 0;
}
