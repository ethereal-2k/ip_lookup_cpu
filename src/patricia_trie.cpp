// patricia_trie_bench.cpp
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include <chrono>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdint>

static const char* PREFIX_FILE   = "data/prefix_table.csv";
static const char* IP_FILE       = "data/generated_ips.csv";
static const char* MATCH_FILE    = "benchmarks/match_pat.csv";
static const char* RESULTS_FILE  = "benchmarks/results_pat.csv";

static inline uint32_t mask_from_len(uint8_t len){ return (len==0)?0U:(~0U << (32-len)); }
static inline uint32_t ip_str_to_uint(const std::string& s){ in_addr a{}; inet_pton(AF_INET,s.c_str(),&a); return ntohl(a.s_addr); }
static inline std::vector<uint8_t> hex_to_bytes(const std::string& h){
    std::vector<uint8_t> out; out.reserve(h.size()/2);
    for(size_t i=0;i+1<h.size();i+=2) out.push_back(uint8_t(std::stoi(h.substr(i,2),nullptr,16)));
    return out;
}
static inline std::string bytes_to_hex(const std::vector<uint8_t>& b){
    std::ostringstream oss; for(auto v: b) oss<<std::hex<<std::setw(2)<<std::setfill('0')<<int(v); return oss.str();
}
static inline bool file_exists(const char* p){ std::ifstream f(p); return f.good(); }
static inline auto now(){ return std::chrono::high_resolution_clock::now(); }
static inline double secs_since(std::chrono::high_resolution_clock::time_point t0){
    return std::chrono::duration<double>(now()-t0).count();
}
static inline size_t current_rss_bytes(){
    std::ifstream statm("/proc/self/statm"); size_t sz=0,res=0; if(statm) statm>>sz>>res;
    return res*size_t(sysconf(_SC_PAGESIZE));
}
static inline double bytes_to_mb(size_t b){ return double(b)/(1024.0*1024.0); }

struct PatriciaNode {
    // For internal nodes: bit_index >= 0, has_key maybe true (if a prefix ends here)
    // For leaves: bit_index == -1, holds a prefix & key
    uint32_t prefix = 0;       // aligned
    uint8_t  prefix_len = 0;
    int      bit_index = -1;   // split bit, -1 for leaf
    bool     has_key = false;
    std::vector<uint8_t> key;
    PatriciaNode* left = nullptr;
    PatriciaNode* right = nullptr;
};

class PatriciaTrie {
public:
    ~PatriciaTrie(){ destroy(root); }

    void insert(uint32_t net, uint8_t len, std::vector<uint8_t>&& key){
        net &= mask_from_len(len);
        root = insert_into(root, net, len, std::move(key));
        ++inserted_;
    }

    const std::vector<uint8_t>* lpm(uint32_t ip) const {
        const std::vector<uint8_t>* best = nullptr;
        const PatriciaNode* n = root;
        while(n){
            if(n->has_key && match_prefix(ip, n->prefix, n->prefix_len))
                best = &n->key;
            if(n->bit_index == -1) break;
            int bit = (ip >> (31 - n->bit_index)) & 1;
            n = (bit==0)? n->left : n->right;
        }
        return best;
    }

private:
    PatriciaNode* root = nullptr;
    size_t inserted_ = 0;

    static bool match_prefix(uint32_t ip, uint32_t pfx, uint8_t len){
        if(len==0) return true;
        uint32_t m = mask_from_len(len);
        return (ip & m) == (pfx & m);
    }

    static int first_differing_bit(uint32_t a, uint32_t b){
        uint32_t x = a ^ b;
        if(x==0) return -1;
        for(int i=0;i<32;++i){
            if( ((x >> (31 - i)) & 1) ) return i;
        }
        return -1;
    }

    static PatriciaNode* make_leaf(uint32_t net, uint8_t len, std::vector<uint8_t>&& key){
        auto* n = new PatriciaNode();
        n->prefix = net; n->prefix_len = len; n->bit_index = -1;
        n->has_key = true; n->key = std::move(key);
        return n;
    }

    static PatriciaNode* make_internal(int bit_index){
        auto* n = new PatriciaNode();
        n->bit_index = bit_index;
        return n;
    }

    // Insert (net,len) under node. Returns (possibly new) subtree root.
    static PatriciaNode* insert_into(PatriciaNode* node, uint32_t net, uint8_t len, std::vector<uint8_t>&& key){
        if(!node) return make_leaf(net, len, std::move(key));

        if(node->bit_index == -1){
            // Existing leaf (p,q); new (a,b)
            uint32_t p = node->prefix; uint8_t q = node->prefix_len;

            if(p == net && q == len){
                // Exact same prefix: overwrite key
                node->has_key = true;
                node->key = std::move(key);
                return node;
            }

            // Check containment (ancestor/descendant)
            bool a_contains_p = match_prefix(p, net, len); // new contains old?
            bool p_contains_a = match_prefix(net, p, q);   // old contains new?

            if(p_contains_a && len > q){
                // New is more specific than existing leaf
                // Create internal at split bit = q
                int split = q; // next bit after old prefix
                auto* internal = make_internal(split);
                // The old prefix should be kept at the internal node as a prefix endpoint
                internal->has_key = true;
                internal->prefix = p;
                internal->prefix_len = q;
                internal->key = std::move(node->key);

                // Attach the old leaf further down its branch if needed? For Patricia,
                // we can make the old leaf unnecessary since internal already holds its key.
                // But node might have been only a leaf; we can reuse node as the child that
                // represents anything extending the old prefix bit=0/1 matching:
                // Simpler: make a new leaf for the new prefix and discard node as a leaf.
                delete node;

                int new_bit = (net >> (31 - split)) & 1;
                auto* new_leaf = make_leaf(net, len, std::move(key));
                if(new_bit==0){ internal->left = new_leaf; internal->right = nullptr; }
                else          { internal->left = nullptr;  internal->right = new_leaf; }
                return internal;
            }

            if(a_contains_p && q > len){
                // New is less specific (ancestor) than existing leaf
                // Create internal at split bit = len
                int split = len;
                auto* internal = make_internal(split);
                // internal node carries the new prefix key
                internal->has_key = true;
                internal->prefix = net;
                internal->prefix_len = len;
                internal->key = std::move(key);

                // Existing leaf goes under the branch of its bit at 'split'
                int old_bit = (p >> (31 - split)) & 1;
                if(old_bit==0){ internal->left = node; internal->right = nullptr; }
                else          { internal->left = nullptr; internal->right = node; }
                return internal;
            }

            // Neither contains the other: split at first differing bit of aligned nets
            int d = first_differing_bit(p, net);
            auto* internal = make_internal(d);

            int bit_old = (p   >> (31 - d)) & 1;
            int bit_new = (net >> (31 - d)) & 1;

            auto* new_leaf = make_leaf(net, len, std::move(key));
            if(bit_new==0){
                internal->left  = new_leaf;
                internal->right = node;
            }else{
                internal->left  = node;
                internal->right = new_leaf;
            }
            // No key at internal (unless one equals a prefix ending here, which it doesn't)
            return internal;
        }

        // Internal node: descend by its split bit
        int bit = (net >> (31 - node->bit_index)) & 1;
        if(bit==0) node->left  = insert_into(node->left,  net, len, std::move(key));
        else       node->right = insert_into(node->right, net, len, std::move(key));
        return node;
    }

    static void destroy(PatriciaNode* n){
        if(!n) return;
        destroy(n->left); destroy(n->right); delete n;
    }
};

// ---------- Batch & benchmark like your other programs ----------
int main(int argc, char* argv[]){
    bool write_hex = false;
    for(int i=1;i<argc;++i){
        std::string a = argv[i];
        if(a=="-chk"||a=="--chk") write_hex=true;
        else if(a=="-h"||a=="--help"){
            std::cout<<"Usage: "<<argv[0]<<" [-chk]\n";
            return 0;
        }
    }

    // Phase A: load prefixes
    if(!file_exists(PREFIX_FILE)){ std::cerr<<"Error: cannot open "<<PREFIX_FILE<<"\n"; return 1; }
    auto tA0 = now(); size_t rssA0 = current_rss_bytes();

    std::ifstream pf(PREFIX_FILE);
    std::string line; std::getline(pf,line); // header
    struct Rec{ uint32_t net; uint8_t len; std::vector<uint8_t> key; };
    std::vector<Rec> recs; recs.reserve(200000);
    size_t num_prefixes=0;
    while(std::getline(pf,line)){
        std::istringstream ss(line);
        std::string pfx, key_hex;
        if(!std::getline(ss,pfx,',')||!std::getline(ss,key_hex)) continue;
        auto slash=pfx.find('/'); if(slash==std::string::npos) continue;
        uint32_t net = ip_str_to_uint(pfx.substr(0,slash));
        uint8_t len = uint8_t(std::stoi(pfx.substr(slash+1)));
        net &= mask_from_len(len);
        recs.push_back({net,len,hex_to_bytes(key_hex)});
        ++num_prefixes;
    }
    double prefix_load_s = secs_since(tA0);
    size_t rssA1 = current_rss_bytes();
    size_t mem_prefix_array_bytes = (rssA1>rssA0? rssA1-rssA0:0);

    // Phase B: build trie
    auto tB0 = now(); size_t rssB0 = current_rss_bytes();
    PatriciaTrie trie;
    for(auto& r: recs) trie.insert(r.net, r.len, std::move(r.key));
    double build_ds_s = secs_since(tB0);
    size_t rssB1 = current_rss_bytes();
    size_t mem_ds_bytes = (rssB1>rssB0? rssB1-rssB0:0);

    recs.clear(); recs.shrink_to_fit();

    // Phase C: load IPs
    if(!file_exists(IP_FILE)){ std::cerr<<"Error: cannot open "<<IP_FILE<<"\n"; return 1; }
    auto tC0=now(); size_t rssC0=current_rss_bytes();
    std::ifstream ipf(IP_FILE); std::getline(ipf,line); // header
    std::vector<uint32_t> ips; ips.reserve(1<<20);
    std::vector<std::string> ip_strs; ip_strs.reserve(1<<20);
    while(std::getline(ipf,line)){
        std::istringstream ss(line);
        std::string ip_s, dump;
        if(!std::getline(ss,ip_s,',')) continue;
        std::getline(ss,dump);
        ip_strs.push_back(ip_s);
        ips.push_back(ip_str_to_uint(ip_s));
    }
    double ip_load_s = secs_since(tC0);
    size_t rssC1 = current_rss_bytes();
    size_t mem_ip_array_bytes = (rssC1>rssC0? rssC1-rssC0:0);

    // Phase D: lookup
    auto tD0 = now();
    std::vector<std::pair<std::string,std::string>> results; results.reserve(ips.size());
    for(size_t i=0;i<ips.size();++i){
        auto* k = trie.lpm(ips[i]);
        if(write_hex) results.emplace_back(ip_strs[i], k? bytes_to_hex(*k) : std::string("-1"));
        else          results.emplace_back(ip_strs[i], k? std::string("1") : std::string("-1"));
    }
    double lookup_s = secs_since(tD0);

    // Write matches
    {
        std::ofstream out(MATCH_FILE);
        out<<"ip,key\n";
        for(auto& r: results) out<<r.first<<','<<r.second<<'\n';
    }

    // Metrics
    double ns_per_lookup = ips.empty()?0.0 : (lookup_s*1e9/double(ips.size()));
    double lookups_per_s = (lookup_s>0.0)? (double(ips.size())/lookup_s) : 0.0;
    double mem_prefix_array_mb = bytes_to_mb(mem_prefix_array_bytes);
    double mem_ds_mb           = bytes_to_mb(mem_ds_bytes);
    double mem_ip_array_mb     = bytes_to_mb(mem_ip_array_bytes);
    double mem_total_mb        = bytes_to_mb(current_rss_bytes());

    bool need_header = !file_exists(RESULTS_FILE);
    std::ofstream res(RESULTS_FILE, std::ios::app);
    if(need_header){
        res<<"algorithm,prefix_file,ip_file,num_prefixes,num_ips,"
              "prefix_load_s,build_ds_s,ip_load_s,lookup_s,"
              "lookups_per_s,ns_per_lookup,"
              "mem_prefix_array_mb,mem_ds_mb,mem_ip_array_mb,mem_total_mb\n";
    }
    res<< "PatriciaTrie" << ','
       << PREFIX_FILE << ','
       << IP_FILE << ','
       << num_prefixes << ','
       << ips.size() << ','
       << std::fixed << std::setprecision(6)
       << prefix_load_s << ','
       << build_ds_s << ','
       << ip_load_s << ','
       << lookup_s << ','
       << std::setprecision(2)
       << lookups_per_s << ','
       << ns_per_lookup << ','
       << std::setprecision(2)
       << mem_prefix_array_mb << ','
       << mem_ds_mb << ','
       << mem_ip_array_mb << ','
       << mem_total_mb << '\n';

    return 0;
}
