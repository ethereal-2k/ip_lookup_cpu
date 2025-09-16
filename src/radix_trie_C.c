#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>

// ------------------------- Paths -------------------------
static const char* PREFIX_FILE   = "data/prefix_table.csv";
static const char* IP_FILE       = "data/generated_ips.csv";
static const char* MATCH_FILE    = "benchmarks/match_radix_C.csv";
static const char* RESULTS_FILE  = "benchmarks/results_radix.csv";

// ------------------------- Helpers -----------------------
static inline uint32_t ip_str_to_uint(const char* ip_str) {
    struct in_addr addr;
    inet_pton(AF_INET, ip_str, &addr);
    return ntohl(addr.s_addr);
}
static inline uint32_t mask_from_len(uint8_t len) {
    return (len == 0) ? 0U : (~0U << (32 - len));
}
static inline unsigned char* hex_to_bytes(const char* hex, size_t* out_len) {
    size_t len = strlen(hex) / 2;
    unsigned char* out = malloc(len);
    for (size_t i = 0; i < len; i++) {
        char buf[3] = { hex[2*i], hex[2*i+1], 0 };
        out[i] = (unsigned char)strtol(buf, NULL, 16);
    }
    *out_len = len;
    return out;
}
static inline char* bytes_to_hex(const unsigned char* bytes, size_t len) {
    char* out = malloc(len*2 + 1);
    for (size_t i = 0; i < len; i++)
        sprintf(out + 2*i, "%02x", bytes[i]);
    out[len*2] = '\0';
    return out;
}
static inline int file_exists(const char* path) {
    FILE* f = fopen(path, "r");
    if (f) { fclose(f); return 1; }
    return 0;
}
static inline double now_secs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static inline size_t current_rss_bytes() {
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    size_t size=0, resident=0;
    fscanf(f, "%zu %zu", &size, &resident);
    fclose(f);
    return resident * (size_t)sysconf(_SC_PAGESIZE);
}
static inline double bytes_to_mb(size_t b) { return (double)b / (1024.0*1024.0); }

// ------------------------- Data --------------------------
typedef struct Node {
    struct Node* child[2];
    int has_key;
    unsigned char* key;
    size_t key_len;
} Node;

typedef struct {
    Node* root;
    size_t inserted;
} BinaryTrie;

typedef struct {
    uint32_t net;
    uint8_t len;
    unsigned char* key;
    size_t key_len;
} PrefixRec;

typedef struct {
    uint32_t ip;
    char* ip_str;
} IpRec;

static Node* new_node() {
    Node* n = calloc(1, sizeof(Node));
    return n;
}
static BinaryTrie* trie_create() {
    BinaryTrie* t = malloc(sizeof(BinaryTrie));
    t->root = new_node();
    t->inserted = 0;
    return t;
}
static void trie_destroy_node(Node* n) {
    if (!n) return;
    trie_destroy_node(n->child[0]);
    trie_destroy_node(n->child[1]);
    if (n->has_key && n->key) free(n->key);
    free(n);
}
static void trie_destroy(BinaryTrie* t) {
    trie_destroy_node(t->root);
    free(t);
}
static void trie_insert(BinaryTrie* t, uint32_t net, uint8_t len,
                        unsigned char* key, size_t key_len) {
    if (len > 0) net &= mask_from_len(len);
    Node* n = t->root;
    for (int i = 0; i < len; i++) {
        int bit = (net >> (31 - i)) & 1;
        if (!n->child[bit]) n->child[bit] = new_node();
        n = n->child[bit];
    }
    if (n->has_key && n->key) free(n->key);
    n->has_key = 1;
    n->key = key;
    n->key_len = key_len;
    t->inserted++;
}
static const Node* trie_lpm(const BinaryTrie* t, uint32_t ip) {
    const Node* best = NULL;
    const Node* n = t->root;
    if (n->has_key) best = n;
    for (int i = 0; i < 32 && n; i++) {
        int bit = (ip >> (31 - i)) & 1;
        n = n->child[bit];
        if (!n) break;
        if (n->has_key) best = n;
    }
    return best;
}

// ------------------------- Main --------------------------
int main(int argc, char* argv[]) {
    int write_hex = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-chk") || !strcmp(argv[i], "--chk")) write_hex = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("Usage: %s [-chk]\n", argv[0]);
            return 0;
        }
    }

    if (!file_exists(PREFIX_FILE) || !file_exists(IP_FILE)) {
        fprintf(stderr, "Error: input files missing.\n");
        return 1;
    }

    size_t rss_baseline = current_rss_bytes();

    // -------- Phase A: Load prefixes --------
    double tA0 = now_secs();
    size_t rssA0 = current_rss_bytes();

    FILE* pf = fopen(PREFIX_FILE, "r");
    char line[8192];
    fgets(line, sizeof(line), pf); // skip header

    PrefixRec* prefixes = malloc(200000 * sizeof(PrefixRec));
    size_t num_prefixes = 0;

    while (fgets(line, sizeof(line), pf)) {
        char* prefix_str = strtok(line, ",");
        char* key_hex = strtok(NULL, "\n");
        if (!prefix_str || !key_hex) continue;
        char* slash = strchr(prefix_str, '/');
        if (!slash) continue;

        *slash = '\0';
        char* ip_part = prefix_str;
        uint8_t len = (uint8_t)atoi(slash+1);
        uint32_t net = ip_str_to_uint(ip_part) & mask_from_len(len);

        size_t key_len;
        unsigned char* key = hex_to_bytes(key_hex, &key_len);

        prefixes[num_prefixes].net = net;
        prefixes[num_prefixes].len = len;
        prefixes[num_prefixes].key = key;
        prefixes[num_prefixes].key_len = key_len;
        num_prefixes++;
    }
    fclose(pf);

    double prefix_load_s = now_secs() - tA0;
    size_t rssA1 = current_rss_bytes();
    size_t mem_prefix_array_bytes = (rssA1 > rssA0 ? rssA1 - rssA0 : 0);

    // -------- Phase B: Build trie --------
    double tB0 = now_secs();
    size_t rssB0 = current_rss_bytes();

    BinaryTrie* trie = trie_create();
    for (size_t i = 0; i < num_prefixes; i++) {
        trie_insert(trie, prefixes[i].net, prefixes[i].len,
                    prefixes[i].key, prefixes[i].key_len);
    }
    double build_ds_s = now_secs() - tB0;
    size_t rssB1 = current_rss_bytes();
    size_t mem_ds_bytes = (rssB1 > rssB0 ? rssB1 - rssB0 : 0);

    free(prefixes); // free prefix array

    // -------- Phase C: Load IPs --------
    double tC0 = now_secs();
    size_t rssC0 = current_rss_bytes();

    FILE* ipf = fopen(IP_FILE, "r");
    fgets(line, sizeof(line), ipf); // skip header

    size_t ip_capacity = 1<<20;
    size_t num_ips = 0;
    IpRec* ips = malloc(ip_capacity * sizeof(IpRec));

    while (fgets(line, sizeof(line), ipf)) {
        char* ip_str = strtok(line, ",");
        strtok(NULL, "\n");
        if (!ip_str) continue;
        if (num_ips >= ip_capacity) {
            ip_capacity *= 2;
            ips = realloc(ips, ip_capacity * sizeof(IpRec));
        }
        ips[num_ips].ip = ip_str_to_uint(ip_str);
        ips[num_ips].ip_str = strdup(ip_str);
        num_ips++;
    }
    fclose(ipf);

    double ip_load_s = now_secs() - tC0;
    size_t rssC1 = current_rss_bytes();
    size_t mem_ip_array_bytes = (rssC1 > rssC0 ? rssC1 - rssC0 : 0);

    // -------- Phase D: Lookup timing --------
    double tD0 = now_secs();

    char** results = malloc(num_ips * sizeof(char*));
    for (size_t i = 0; i < num_ips; i++) {
        const Node* n = trie_lpm(trie, ips[i].ip);
        if (n) {
            if (write_hex) {
                results[i] = bytes_to_hex(n->key, n->key_len);
            } else {
                results[i] = strdup("1");
            }
        } else {
            results[i] = strdup("-1");
        }
    }
    double lookup_s = now_secs() - tD0;

    double ns_per_lookup = num_ips ? (lookup_s * 1e9 / (double)num_ips) : 0.0;
    double lookups_per_s = (lookup_s > 0.0) ? (num_ips / lookup_s) : 0.0;

    // -------- Phase E: Write matches --------
    FILE* outf = fopen(MATCH_FILE, "w");
    fprintf(outf, "ip,key\n");
    for (size_t i = 0; i < num_ips; i++) {
        fprintf(outf, "%s,%s\n", ips[i].ip_str, results[i]);
        free(results[i]);
        free(ips[i].ip_str);
    }
    fclose(outf);

    free(results);
    free(ips);

    // -------- Final mem & write results CSV --------
    size_t rss_total_bytes = current_rss_bytes();
    double mem_prefix_array_mb = bytes_to_mb(mem_prefix_array_bytes);
    double mem_ds_mb           = bytes_to_mb(mem_ds_bytes);
    double mem_ip_array_mb     = bytes_to_mb(mem_ip_array_bytes);
    double mem_total_mb        = bytes_to_mb(rss_total_bytes);

    int need_header = !file_exists(RESULTS_FILE);
    FILE* res = fopen(RESULTS_FILE, "a");
    if (res) {
        if (need_header) {
            fprintf(res,
              "algorithm,prefix_file,ip_file,num_prefixes,num_ips,"
              "prefix_load_s,build_ds_s,ip_load_s,lookup_s,"
              "lookups_per_s,ns_per_lookup,"
              "mem_prefix_array_mb,mem_ds_mb,mem_ip_array_mb,mem_total_mb\n");
        }
        fprintf(res,
          "BinaryRadixTrie_C,%s,%s,%zu,%zu,"
          "%.6f,%.6f,%.6f,%.6f,"
          "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
          PREFIX_FILE, IP_FILE, num_prefixes, num_ips,
          prefix_load_s, build_ds_s, ip_load_s, lookup_s,
          lookups_per_s, ns_per_lookup,
          mem_prefix_array_mb, mem_ds_mb, mem_ip_array_mb, mem_total_mb);
        fclose(res);
    }

    printf("Done: %zu prefixes, %zu IPs, %.2f Mpps\n",
           num_prefixes, num_ips, lookups_per_s/1e6);

    trie_destroy(trie);
    return 0;
}
