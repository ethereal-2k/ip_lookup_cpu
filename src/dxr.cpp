// src/dxr_16_8_8.cpp
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
// ---------------- Paths ----------------
static const char* PREFIX_FILE   = "data/prefix_table.csv";
static const char* IP_FILE       = "data/generated_ips.csv";
static const char* MATCH_FILE    = "benchmarks/match_dxr.csv";
static const char* RESULTS_FILE  = "benchmarks/results_dxr.csv";

// ---------------- Utils ----------------
static inline uint32_t mask_from_len(uint8_t len){ return (len==0)?0U:(~0U << (32-len)); }
static inline uint32_t ip_str_to_uint(const std::string& s){ in_addr a{}; inet_pton(AF_INET,s.c_str(),&a); return ntohl(a.s_addr); }
static inline bool file_exists(const char* p){ std::ifstream f(p); return f.good(); }

static inline auto now(){ return std::chrono::high_resolution_clock::now(); }
static inline double secs_since(std::chrono::high_resolution_clock::time_point t){ return std::chrono::duration<double>(now()-t).count(); }

static inline size_t rss_bytes(){
    std::ifstream statm("/proc/self/statm"); size_t sz=0,res=0; if(statm) statm>>sz>>res;
    return res * static_cast<size_t>(sysconf(_SC_PAGESIZE));
}
static inline double to_mb(size_t b){ return double(b)/(1024.0*1024.0); }

static inline std::vector<uint8_t> hex_to_bytes(const std::string& h){
    std::vector<uint8_t> out; out.reserve(h.size()/2);
    for(size_t i=0;i+1<h.size(); i+=2) out.push_back(uint8_t(std::stoi(h.substr(i,2), nullptr, 16)));
    return out;
}
static inline std::string bytes_to_hex(const uint8_t* key, int len=64){
    std::ostringstream oss;
    for(int i=0;i<len;++i) oss<<std::hex<<std::setw(2)<<std::setfill('0')<<int(key[i]);
    return oss.str();
}

// ---------------- Key pool (dedup) ----------------
static std::unordered_map<std::string, uint8_t*> g_key_pool;

static inline uint8_t* get_or_create_key(const std::string& hex){
    auto it = g_key_pool.find(hex);
    if(it != g_key_pool.end()) return it->second;
    // expect 128 hex chars (64 bytes)
    std::vector<uint8_t> tmp = hex_to_bytes(hex);
    if(tmp.size() != 64) return nullptr;
    uint8_t* p = new uint8_t[64];
    std::memcpy(p, tmp.data(), 64);
    g_key_pool.emplace(hex, p);
    return p;
}

// ---------------- DXR (DIR-16-8-8) ----------------
// Level 1: 2^16 entries of fallback keys for /0..16
// Level 2: per-/16 allocation of 256-entry arrays of keys for /17..24
// Level 3: per-/24 allocation of 256-entry arrays of keys for /25..32
static const int L1_SIZE = 1 << 16;
static const int L2_SIZE = 256;
static const int L3_SIZE = 256;

uint8_t**   L1_keys   = nullptr;          // [2^16] -> key*
uint8_t***  L2_tables = nullptr;          // [2^16] -> (uint8_t*[256]) or nullptr
uint8_t**** L3_tables = nullptr;          // [2^16] -> (uint8_t**[256]) or nullptr
                                          // where L3_tables[top][mid] -> uint8_t*[256] or nullptr

int main(int argc, char* argv[]){
    bool write_hex = false;
    for(int i=1;i<argc;++i){
        std::string a = argv[i];
        if(a=="-chk"||a=="--chk") write_hex = true;
        else if(a=="-h"||a=="--help"){
            std::cout<<"Usage: "<<argv[0]<<" [-chk]\n";
            return 0;
        }
    }

    // -------- Phase A: Load prefixes (batch) --------
    if(!file_exists(PREFIX_FILE)){ std::cerr<<"Error: cannot open "<<PREFIX_FILE<<"\n"; return 1; }
    auto tA0=now(); size_t rA0=rss_bytes();

    struct PRec{ uint32_t base; uint8_t len; uint8_t* key; };
    std::vector<PRec> prefixes; prefixes.reserve(200000);

    std::ifstream pf(PREFIX_FILE);
    std::string line; std::getline(pf, line); // header "prefix,key"

    size_t num_prefixes=0;
    while(std::getline(pf, line)){
        std::istringstream ss(line);
        std::string pfx, khex;
        if(!std::getline(ss, pfx, ',')) continue;
        if(!std::getline(ss, khex)) continue;

        auto slash = pfx.find('/');
        if(slash == std::string::npos) continue;
        uint32_t net = ip_str_to_uint(pfx.substr(0, slash));
        uint8_t  len = (uint8_t)std::stoi(pfx.substr(slash+1));
        net &= mask_from_len(len);

        uint8_t* key = get_or_create_key(khex);
        if(!key) continue;

        prefixes.push_back({net, len, key});
        ++num_prefixes;
    }

    double prefix_load_s = secs_since(tA0);
    double mem_prefix_mb = to_mb(rss_bytes() - rA0);

    // -------- Phase B: Build DXR structure --------
    auto tB0=now(); size_t rB0=rss_bytes();

    // allocate top-level
    L1_keys   = new uint8_t*[L1_SIZE]();    // zeroed
    L2_tables = new uint8_t**[L1_SIZE]();   // nullptrs
    L3_tables = new uint8_t***[L1_SIZE]();  // nullptrs

    auto ensure_L2 = [&](uint32_t top){
        if(!L2_tables[top]) L2_tables[top] = new uint8_t*[L2_SIZE]();
    };
    auto ensure_L3_mid = [&](uint32_t top, uint32_t mid){
        if(!L3_tables[top]) L3_tables[top] = new uint8_t**[L2_SIZE]();
        if(!L3_tables[top][mid]) L3_tables[top][mid] = new uint8_t*[L3_SIZE]();
    };

    for(const auto& rec : prefixes){
        uint32_t net = rec.base;
        uint8_t  len = rec.len;
        uint8_t* key = rec.key;

        if(len <= 16){
            uint32_t start = net >> 16;
            uint32_t fill  = 1u << (16 - len);
            for(uint32_t i=0;i<fill;++i){
                uint32_t idx = start + i;
                if(!L1_keys[idx]) L1_keys[idx] = key;  // only if empty (less-specific fallback)
            }
        } else if(len <= 24){
            uint32_t top   = net >> 16;
            uint32_t mid_s = (net >> 8) & 0xFFu;
            uint32_t fill  = 1u << (24 - len);
            ensure_L2(top);
            for(uint32_t j=0;j<fill;++j){
                uint32_t mid = mid_s + j;
                if(!L2_tables[top][mid]) L2_tables[top][mid] = key; // set if empty
            }
        } else {
            // /25..32
            uint32_t top   = net >> 16;
            uint32_t mid   = (net >> 8) & 0xFFu;
            uint32_t low_s = net & 0xFFu;
            uint32_t fill  = 1u << (32 - len);
            ensure_L3_mid(top, mid);
            for(uint32_t k=0;k<fill;++k){
                uint32_t low = low_s + k;
                if(!L3_tables[top][mid][low]) L3_tables[top][mid][low] = key; // set if empty
            }
        }
    }

    double build_ds_s = secs_since(tB0);
    double mem_ds_mb  = to_mb(rss_bytes() - rB0);

    // Optionally free the vector to isolate DS memory
    // (keys remain owned by g_key_pool and referenced by DS)
    prefixes.clear(); prefixes.shrink_to_fit();

    // -------- Phase C: Load IPs (batch) --------
    if(!file_exists(IP_FILE)){ std::cerr<<"Error: cannot open "<<IP_FILE<<"\n"; return 1; }
    auto tC0=now(); size_t rC0=rss_bytes();

    std::ifstream ipf(IP_FILE);
    std::getline(ipf, line); // header "ip,used_prefix"

    std::vector<std::string> ip_strs; ip_strs.reserve(1<<20);
    std::vector<uint32_t>    ips;     ips.reserve(1<<20);

    while(std::getline(ipf, line)){
        std::istringstream ss(line);
        std::string ip_s, dump;
        if(!std::getline(ss, ip_s, ',')) continue;
        std::getline(ss, dump);
        ip_strs.push_back(ip_s);
        ips.push_back(ip_str_to_uint(ip_s));
    }

    double ip_load_s = secs_since(tC0);
    double mem_ip_mb = to_mb(rss_bytes() - rC0);

    // -------- Phase D: Lookup --------
    auto tD0=now();

    std::vector<std::pair<std::string,std::string>> results; results.reserve(ips.size());
    for(size_t i=0;i<ips.size();++i){
        uint32_t ip  = ips[i];
        uint32_t top = ip >> 16;
        uint32_t mid = (ip >> 8) & 0xFFu;
        uint32_t low = ip & 0xFFu;

        uint8_t* key = nullptr;
        // most specific first
        if(L3_tables[top] && L3_tables[top][mid] && L3_tables[top][mid][low]){
            key = L3_tables[top][mid][low];
        } else if(L2_tables[top] && L2_tables[top][mid]){
            key = L2_tables[top][mid];
        } else if(L1_keys[top]){
            key = L1_keys[top];
        }

        if(write_hex) results.emplace_back(ip_strs[i], key ? bytes_to_hex(key) : std::string("-1"));
        else          results.emplace_back(ip_strs[i], key ? std::string("1")   : std::string("-1"));
    }

    double lookup_s = secs_since(tD0);
    double ns_per_lookup = ips.empty()? 0.0 : (lookup_s*1e9 / double(ips.size()));
    double lookups_per_s = (lookup_s > 0.0) ? (double(ips.size()) / lookup_s) : 0.0;

    // -------- Write match file --------
    {
        std::ofstream out(MATCH_FILE);
        out<<"ip,key\n";
        for(auto& r : results) out<<r.first<<','<<r.second<<'\n';
    }

    // -------- Metrics CSV (MB) --------
    double mem_total_mb = to_mb(rss_bytes());
    bool need_header = !file_exists(RESULTS_FILE);
    std::ofstream res(RESULTS_FILE, std::ios::app);
    if(need_header){
        res<<"algorithm,prefix_file,ip_file,num_prefixes,num_ips,"
              "prefix_load_s,build_ds_s,ip_load_s,lookup_s,"
              "lookups_per_s,ns_per_lookup,"
              "mem_prefix_array_mb,mem_ds_mb,mem_ip_array_mb,mem_total_mb\n";
    }
    res<<"DXR-16-8-8"<<','
       <<PREFIX_FILE<<','<<IP_FILE<<','
       <<num_prefixes<<','<<ips.size()<<','
       <<std::fixed<<std::setprecision(6)
       <<prefix_load_s<<','<<build_ds_s<<','<<ip_load_s<<','<<lookup_s<<','
       <<std::setprecision(2)
       <<lookups_per_s<<','<<ns_per_lookup<<','
       <<std::setprecision(2)
       <<mem_prefix_mb<<','<<mem_ds_mb<<','<<mem_ip_mb<<','<<mem_total_mb<<'\n';

    // -------- Cleanup (keys + tables) --------
    for(auto& kv : g_key_pool) delete[] kv.second;
    g_key_pool.clear();

    if(L3_tables){
        for(int top=0; top<L1_SIZE; ++top){
            if(L3_tables[top]){
                for(int mid=0; mid<L2_SIZE; ++mid){
                    if(L3_tables[top][mid]) delete[] L3_tables[top][mid];
                }
                delete[] L3_tables[top];
            }
        }
        delete[] L3_tables;
    }
    if(L2_tables){
        for(int top=0; top<L1_SIZE; ++top){
            if(L2_tables[top]) delete[] L2_tables[top];
        }
        delete[] L2_tables;
    }
    if(L1_keys) delete[] L1_keys;

    return 0;
}
