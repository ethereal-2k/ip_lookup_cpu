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
#include <unordered_set>
#include <algorithm>
#include <unistd.h>   // sysconf

// ------------------------- Config / constants -------------------------
static const int MAIN_TABLE_SIZE = 1 << 24;  // 2^24
static const int SUBTABLE_SIZE   = 256;

// File paths (relative to repo root)
static const char* PREFIX_FILE   = "data/prefix_table.csv";
static const char* IP_FILE       = "data/generated_ips.csv";
static const char* MATCH_FILE    = "benchmarks/match_dir24_8.csv";
static const char* RESULTS_FILE  = "benchmarks/results_dir24_8.csv";

// ------------------------- Memory / timing helpers --------------------
size_t current_rss_bytes() {
    // Linux: /proc/self/statm (resident pages * page size)
    std::ifstream statm("/proc/self/statm");
    size_t size=0, resident=0;
    if (statm) statm >> size >> resident;
    return resident * static_cast<size_t>(sysconf(_SC_PAGESIZE));
}
inline auto now() {
    return std::chrono::high_resolution_clock::now();
}
inline double seconds_since(std::chrono::high_resolution_clock::time_point t0) {
    return std::chrono::duration<double>(now() - t0).count();
}
inline double bytes_to_mb(size_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

// ------------------------- Utilities ---------------------------------
uint32_t ip_str_to_uint(const std::string& ip_str) {
    in_addr addr{};
    inet_pton(AF_INET, ip_str.c_str(), &addr);
    return ntohl(addr.s_addr);
}
std::string uint_to_ip_str(uint32_t ip) {
    in_addr addr{};
    addr.s_addr = htonl(ip);
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, buf, INET_ADDRSTRLEN);
    return std::string(buf);
}
uint32_t mask_from_len(uint8_t len) {
    return (len == 0) ? 0U : (~0U << (32 - len));
}
std::string bytes_to_hex(const uint8_t* key, int len = 64) {
    std::ostringstream oss;
    for (int i = 0; i < len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(key[i]);
    }
    return oss.str();
}
bool file_exists(const char* path) {
    std::ifstream f(path);
    return f.good();
}

// ------------------------- Data structures ---------------------------
struct PrefixRec {
    uint32_t base_ip;  // aligned network address
    uint8_t  len;      // prefix length
    uint8_t* key;      // pointer to 64-byte key (deduped)
};
std::unordered_map<std::string, uint8_t*> g_key_pool;

// DIR-24-8 tables
uint8_t**  main_table  = nullptr;           // [2^24] -> key* (for <= /24)
uint8_t*** sub_tables  = nullptr;           // [2^24] -> array[256] of key* (for > /24)

// ------------------------- Key handling -------------------------------
uint8_t* get_or_create_key(const std::string& hex) {
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

// ------------------------- Main --------------------------------------
int main(int argc, char* argv[]) {
    // Check for -chk flag to output hex keys
    bool write_hex = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-chk" || arg == "--chk") {
            write_hex = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [-chk]\n"
                      << "  -chk   Write hex keys to match file (slower)\n";
            return 0;
        }
    }

    // ----------------- Phase 0: Baseline memory -----------------------
    size_t rss_baseline = current_rss_bytes();

    // ----------------- Phase A: Load Prefixes (batch) -----------------
    if (!file_exists(PREFIX_FILE)) {
        std::cerr << "Error: cannot open " << PREFIX_FILE << "\n";
        return 1;
    }

    auto tA0 = now();
    size_t rssA0 = current_rss_bytes();

    std::ifstream fib(PREFIX_FILE);
    std::string line;
    std::getline(fib, line); // skip header "prefix,key"

    std::vector<PrefixRec> prefixes;
    prefixes.reserve(200000); // guess; adjust as needed

    size_t num_prefixes = 0;
    while (std::getline(fib, line)) {
        std::istringstream ss(line);
        std::string prefix_str, key_hex;
        if (!std::getline(ss, prefix_str, ',')) continue;
        if (!std::getline(ss, key_hex)) continue;

        auto slash = prefix_str.find('/');
        if (slash == std::string::npos) continue;

        std::string ip_part = prefix_str.substr(0, slash);
        uint8_t len = static_cast<uint8_t>(std::stoi(prefix_str.substr(slash + 1)));
        uint32_t base_ip = ip_str_to_uint(ip_part) & mask_from_len(len);

        uint8_t* key_ptr = get_or_create_key(key_hex);
        prefixes.push_back({base_ip, len, key_ptr});
        ++num_prefixes;
    }

    double prefix_load_s = seconds_since(tA0);
    size_t rssA1 = current_rss_bytes();
    size_t mem_prefix_array_bytes = (rssA1 > rssA0 ? rssA1 - rssA0 : 0);

    // ----------------- Phase B: Build DS (DIR-24-8) -------------------
    auto tB0 = now();
    size_t rssB0 = current_rss_bytes();

    // allocate top-level tables
    main_table = new uint8_t*[MAIN_TABLE_SIZE]();
    sub_tables = new uint8_t**[MAIN_TABLE_SIZE]();

    // Fill tables from prefixes
    for (const auto& rec : prefixes) {
        const uint32_t ip_aligned = rec.base_ip;
        const uint8_t len = rec.len;
        if (len > 32) continue;  // Skip invalid prefix lengths
        uint8_t* key = rec.key;

        if (len <= 24) {
            const uint32_t start = ip_aligned >> 8;
            const uint32_t fill  = 1u << (24 - len);
            for (uint32_t i = 0; i < fill; ++i) {
                if (!main_table[start + i]) {
                    main_table[start + i] = key;
                }
            }
        } else {
            const uint32_t count = 1u << (32 - len);
            for (uint32_t off = 0; off < count; ++off) {
                uint32_t ip_full  = ip_aligned + off;
                uint32_t main_idx = ip_full >> 8;
                uint8_t  sub_idx  = static_cast<uint8_t>(ip_full & 0xFF);

                if (!sub_tables[main_idx]) {
                    sub_tables[main_idx] = new uint8_t*[SUBTABLE_SIZE]();
                }
                if (!sub_tables[main_idx][sub_idx]) {
                    sub_tables[main_idx][sub_idx] = key;
                }
            }
        }
    }

    double build_ds_s = seconds_since(tB0);
    size_t rssB1 = current_rss_bytes();
    size_t mem_ds_bytes = (rssB1 > rssB0 ? rssB1 - rssB0 : 0);

    // Optional: free prefix array to observe DS-only memory
    prefixes.clear();
    prefixes.shrink_to_fit();

    // ----------------- Phase C: Load IPs (batch) ----------------------
    if (!file_exists(IP_FILE)) {
        std::cerr << "Error: cannot open " << IP_FILE << "\n";
        return 1;
    }

    auto tC0 = now();
    size_t rssC0 = current_rss_bytes();

    std::ifstream ipfile(IP_FILE);
    std::getline(ipfile, line); // skip header "ip,used_prefix"

    std::vector<uint32_t> ips;
    std::vector<std::string> ip_strs;
    ips.reserve(1000000); ip_strs.reserve(1000000); // guess; adjust as needed

    while (std::getline(ipfile, line)) {
        std::istringstream ss(line);
        std::string ip_str, discard;
        if (!std::getline(ss, ip_str, ',')) continue;
        std::getline(ss, discard); // used_prefix (ignored for speed)
        ip_strs.push_back(ip_str);
        ips.push_back(ip_str_to_uint(ip_str));
    }

    double ip_load_s = seconds_since(tC0);
    size_t rssC1 = current_rss_bytes();
    size_t mem_ip_array_bytes = (rssC1 > rssC0 ? rssC1 - rssC0 : 0);

    // ----------------- Phase D: Lookup -------------------------------
    auto tD0 = now();

    std::vector<std::string> results;
    results.reserve(ips.size());

    for (uint32_t ip : ips) {
        uint32_t main_idx = ip >> 8;
        uint8_t  sub_idx  = static_cast<uint8_t>(ip & 0xFF);

        uint8_t* key = nullptr;
        if (sub_tables[main_idx] && sub_tables[main_idx][sub_idx]) {
            key = sub_tables[main_idx][sub_idx];
        } else if (main_table[main_idx]) {
            key = main_table[main_idx];
        }
        if (write_hex) {
            results.emplace_back(key ? bytes_to_hex(key) : "-1");
        } else {
            results.emplace_back(key ? "1" : "-1");
        }
    }

    double lookup_time_s = seconds_since(tD0);
    double ns_per_lookup = (ips.empty() ? 0.0 : (lookup_time_s * 1e9 / static_cast<double>(ips.size())));
    double lookups_per_s = (lookup_time_s > 0.0 ? (static_cast<double>(ips.size()) / lookup_time_s) : 0.0);

    // ----------------- Output matches -------------------------------
    {
        std::ofstream out(MATCH_FILE);
        if (!out.is_open()) {
            std::cerr << "Error: cannot open " << MATCH_FILE << " for writing\n";
        } else {
            out << "ip,key\n";
            for (size_t i = 0; i < ips.size(); ++i) {
                out << ip_strs[i] << "," << results[i] << "\n";
            }
        }
    }

    // ----------------- Final memory totals --------------------------
    size_t rss_total_bytes = current_rss_bytes();

    // Convert memory to MB for output
    double mem_prefix_array_mb = bytes_to_mb(mem_prefix_array_bytes);
    double mem_ds_mb           = bytes_to_mb(mem_ds_bytes);
    double mem_ip_array_mb     = bytes_to_mb(mem_ip_array_bytes);
    double mem_total_mb        = bytes_to_mb(rss_total_bytes);

    // ----------------- Append metrics CSV ---------------------------
    // Columns:
    // algorithm,prefix_file,ip_file,num_prefixes,num_ips,
    // prefix_load_s,build_ds_s,ip_load_s,lookup_s,lookups_per_s,ns_per_lookup,
    // mem_prefix_array_mb,mem_ds_mb,mem_ip_array_mb,mem_total_mb
    const char* algo_name = "DIR-24-8";

    bool write_header = !file_exists(RESULTS_FILE);
    std::ofstream r(RESULTS_FILE, std::ios::app);
    if (!r.is_open()) {
        std::cerr << "Error: cannot open " << RESULTS_FILE << " for writing\n";
    } else {
        r.setf(std::ios::fixed);
        if (write_header) {
            r << "algorithm,prefix_file,ip_file,num_prefixes,num_ips,"
                 "prefix_load_s,build_ds_s,ip_load_s,lookup_s,"
                 "lookups_per_s,ns_per_lookup,"
                 "mem_prefix_array_mb,mem_ds_mb,mem_ip_array_mb,mem_total_mb\n";
        }
        r << algo_name << ","
          << PREFIX_FILE << ","
          << IP_FILE << ","
          << num_prefixes << ","
          << ips.size() << ","
          << std::setprecision(6)
          << prefix_load_s << ","
          << build_ds_s << ","
          << ip_load_s << ","
          << lookup_time_s << ","
          << std::setprecision(2) << lookups_per_s << ","
          << std::setprecision(2) << ns_per_lookup << ","
          << std::setprecision(2)
          << mem_prefix_array_mb << ","
          << mem_ds_mb << ","
          << mem_ip_array_mb << ","
          << mem_total_mb
          << "\n";
    }

    // ----------------- Cleanup -------------------------------------
    for (auto& kv : g_key_pool) delete[] kv.second;
    g_key_pool.clear();

    if (sub_tables) {
        for (int i = 0; i < MAIN_TABLE_SIZE; ++i) {
            if (sub_tables[i]) delete[] sub_tables[i];
        }
        delete[] sub_tables;
    }
    if (main_table) delete[] main_table;

    return 0;
}
