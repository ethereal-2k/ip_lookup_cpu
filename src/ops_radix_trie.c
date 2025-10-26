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
static const char* MATCH_FILE    = "benchmarks/ops_match_radix_C.csv";
static const char* RESULTS_FILE  = "benchmarks/ops_results_radix.csv";

// ------------------------- Helpers -----------------------
static inline uint32_t mask_from_len(uint8_t len) {
    return (len == 0) ? 0U : (~0U << (32 - len));
}
static inline double now_secs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
static inline int file_exists(const char* path) {
    FILE* f = fopen(path, "r");
    if (f) { fclose(f); return 1; }
    return 0;
}

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
                        const unsigned char* key, size_t key_len) {
    if (len > 0) net &= mask_from_len(len);
    Node* n = t->root;
    for (int i = 0; i < len; i++) {
        int bit = (net >> (31 - i)) & 1;
        if (!n->child[bit]) n->child[bit] = new_node();
        n = n->child[bit];
    }
    if (n->has_key && n->key) free(n->key);
    n->has_key = 1;
    n->key = malloc(key_len);
    memcpy(n->key, key, key_len);
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
static int trie_delete(BinaryTrie* t, uint32_t net, uint8_t len) {
    if (!t || !t->root) return 0;

    Node* stack[33];
    int path[33];
    Node* n = t->root;
    int depth = 0;

    for (int i = 0; i < len; i++) {
        int bit = (net >> (31 - i)) & 1;
        if (!n->child[bit]) return 0; // not found
        stack[depth] = n;
        path[depth] = bit;
        n = n->child[bit];
        depth++;
    }
    if (!n->has_key) return 0;

    free(n->key);
    n->key = NULL;
    n->key_len = 0;
    n->has_key = 0;
    if (t->inserted > 0) t->inserted--;

    // prune upwards
    for (int d = depth-1; d >= 0; d--) {
        Node* parent = stack[d];
        Node* child = parent->child[path[d]];
        if (child->has_key || child->child[0] || child->child[1]) break;
        free(child);
        parent->child[path[d]] = NULL;
    }
    return 1;
}

// random prefix generator
static PrefixRec* generate_random_prefixes(size_t n) {
    PrefixRec* arr = malloc(n * sizeof(PrefixRec));
    for (size_t i = 0; i < n; i++) {
        uint32_t ip = ((uint32_t)rand() << 16) ^ rand();
        uint8_t len = rand() % 33; // 0..32
        size_t key_len = 16;
        unsigned char* key = malloc(key_len);
        for (size_t j = 0; j < key_len; j++) key[j] = rand() & 0xFF;

        arr[i].net = ip & mask_from_len(len);
        arr[i].len = len;
        arr[i].key = key;
        arr[i].key_len = key_len;
    }
    return arr;
}

// ------------------------- Main --------------------------
int main(int argc, char* argv[]) {
    srand(time(NULL));

    // Build initial trie from prefix file
    FILE* pf = fopen(PREFIX_FILE, "r");
    if (!pf) {
        fprintf(stderr, "Error: cannot open %s\n", PREFIX_FILE);
        return 1;
    }
    char line[8192];
    fgets(line, sizeof(line), pf); // skip header

    BinaryTrie* trie = trie_create();
    size_t num_prefixes = 0;
    while (fgets(line, sizeof(line), pf)) {
        char* prefix_str = strtok(line, ",");
        if (!prefix_str) continue;
        char* slash = strchr(prefix_str, '/');
        if (!slash) continue;

        *slash = '\0';
        char* ip_part = prefix_str;
        uint8_t len = (uint8_t)atoi(slash+1);
        uint32_t net;
        inet_pton(AF_INET, ip_part, &net);
        net = ntohl(net) & mask_from_len(len);

        unsigned char dummy_key[16] = {0};
        trie_insert(trie, net, len, dummy_key, 16);
        num_prefixes++;
    }
    fclose(pf);
    printf("Initial trie built with %zu prefixes.\n", num_prefixes);

    // Ops benchmarks
    size_t N = 100000;
    PrefixRec* rand_prefixes = generate_random_prefixes(N);

    // Insert loop
    double tI0 = now_secs();
    for (size_t i = 0; i < N; i++) {
        trie_insert(trie, rand_prefixes[i].net, rand_prefixes[i].len,
                    rand_prefixes[i].key, rand_prefixes[i].key_len);
    }
    double insert_time = now_secs() - tI0;

    // Load lookup IPs
    FILE* ipf = fopen(IP_FILE, "r");
    if (!ipf) {
        fprintf(stderr, "Error: cannot open %s\n", IP_FILE);
        return 1;
    }
    fgets(line, sizeof(line), ipf); // skip header

    size_t cap_ips = 1<<20, num_ips = 0;
    uint32_t* ips = malloc(cap_ips * sizeof(uint32_t));
    while (fgets(line, sizeof(line), ipf)) {
        char* ip_str = strtok(line, ",");
        if (!ip_str) continue;
        struct in_addr addr;
        inet_pton(AF_INET, ip_str, &addr);
        if (num_ips >= cap_ips) {
            cap_ips *= 2;
            ips = realloc(ips, cap_ips * sizeof(uint32_t));
        }
        ips[num_ips++] = ntohl(addr.s_addr);
    }
    fclose(ipf);

    // Lookup loop
    double tL0 = now_secs();
    volatile int sink = 0;
    for (size_t i = 0; i < num_ips; i++) {
        const Node* n = trie_lpm(trie, ips[i]);
        sink += (n ? 1 : -1);
    }
    double lookup_time = now_secs() - tL0;

    // Delete loop
    double tD0 = now_secs();
    for (size_t i = 0; i < N; i++) {
        trie_delete(trie, rand_prefixes[i].net, rand_prefixes[i].len);
    }
    double delete_time = now_secs() - tD0;

    // Mixed loop (per-op timing inside)
    uint64_t mix_insert_ns = 0, mix_lookup_ns = 0, mix_delete_ns = 0;
    uint64_t tM0 = now_ns();
    for (size_t i = 0; i < N; i++) {
        uint64_t t0 = now_ns();
        trie_insert(trie, rand_prefixes[i].net, rand_prefixes[i].len,
                    rand_prefixes[i].key, rand_prefixes[i].key_len);
        mix_insert_ns += now_ns() - t0;

        t0 = now_ns();
        const Node* n = trie_lpm(trie, ips[i % num_ips]);
        sink += (n ? 1 : -1);
        mix_lookup_ns += now_ns() - t0;

        t0 = now_ns();
        trie_delete(trie, rand_prefixes[i].net, rand_prefixes[i].len);
        mix_delete_ns += now_ns() - t0;
    }
    uint64_t mix_total_ns = now_ns() - tM0;
    double mixed_time = mix_total_ns / 1e9;

    // --- Ratios ---
    // Batch ratios (based on standalone per-op costs)
    double insert_time_per_op = insert_time / N;
    double lookup_time_per_op = lookup_time / num_ips;
    double delete_time_per_op = delete_time / N;
    double total_batch_per_op = insert_time_per_op + lookup_time_per_op + delete_time_per_op;

    double batch_ratio_insert = insert_time_per_op / total_batch_per_op;
    double batch_ratio_lookup = lookup_time_per_op / total_batch_per_op;
    double batch_ratio_delete = delete_time_per_op / total_batch_per_op;

    // Streaming ratios (measured inside mixed loop)
    double stream_ratio_insert = (double)mix_insert_ns / mix_total_ns;
    double stream_ratio_lookup = (double)mix_lookup_ns / mix_total_ns;
    double stream_ratio_delete = (double)mix_delete_ns / mix_total_ns;

    // --- Throughputs ---
    double insert_ops_per_s = N / insert_time;
    double lookup_ops_per_s = num_ips / lookup_time;
    double delete_ops_per_s = N / delete_time;
    double mixed_ops_per_s  = (3.0 * N) / mixed_time;

    // --- Latencies ---
    double insert_ns_per_op = insert_time_per_op * 1e9;
    double lookup_ns_per_op = lookup_time_per_op * 1e9;
    double delete_ns_per_op = delete_time_per_op * 1e9;
    double mixed_ns_per_op  = (mixed_time / N) * 1e9;

    // --- Results CSV ---
    int need_header = !file_exists(RESULTS_FILE);
    FILE* res = fopen(RESULTS_FILE, "a");
    if (res) {
        if (need_header) {
            fprintf(res,
              "algorithm,num_prefixes,num_ops,num_ips,"
              "insert_time,lookup_time,delete_time,mixed_time,"
              "insert_ops_per_s,lookup_ops_per_s,delete_ops_per_s,mixed_ops_per_s,"
              "insert_ns_per_op,lookup_ns_per_op,delete_ns_per_op,mixed_ns_per_op,"
              "batch_ratio_insert,batch_ratio_lookup,batch_ratio_delete,"
              "stream_ratio_insert,stream_ratio_lookup,stream_ratio_delete\n");
        }
        fprintf(res,
          "BinaryRadixTrie_C,%zu,%zu,%zu,"
          "%.9f,%.9f,%.9f,%.9f,"
          "%.2f,%.2f,%.2f,%.2f,"
          "%.2f,%.2f,%.2f,%.2f,"
          "%.4f,%.4f,%.4f,"
          "%.4f,%.4f,%.4f\n",
          num_prefixes, N, num_ips,
          insert_time, lookup_time, delete_time, mixed_time,
          insert_ops_per_s, lookup_ops_per_s, delete_ops_per_s, mixed_ops_per_s,
          insert_ns_per_op, lookup_ns_per_op, delete_ns_per_op, mixed_ns_per_op,
          batch_ratio_insert, batch_ratio_lookup, batch_ratio_delete,
          stream_ratio_insert, stream_ratio_lookup, stream_ratio_delete);
        fclose(res);
    }

    printf("Insert: %.9fs, Lookup: %.9fs, Delete: %.9fs, Mixed: %.9fs\n",
           insert_time, lookup_time, delete_time, mixed_time);
    printf("Batch Ratios: Insert=%.3f Lookup=%.3f Delete=%.3f (sum=1)\n",
           batch_ratio_insert, batch_ratio_lookup, batch_ratio_delete);
    printf("Streaming Ratios (measured in mixed loop): Insert=%.3f Lookup=%.3f Delete=%.3f (sum≈1)\n",
           stream_ratio_insert, stream_ratio_lookup, stream_ratio_delete);

    for (size_t i = 0; i < N; i++) free(rand_prefixes[i].key);
    free(rand_prefixes);
    free(ips);
    trie_destroy(trie);

    return 0;
}
