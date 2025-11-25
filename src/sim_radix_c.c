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
static const char* SIM_FILE      = "benchmarks/sim_radix.csv";

// ------------------------- Helpers -----------------------
static inline uint32_t mask_from_len(uint8_t len) {
    return (len == 0) ? 0U : (~0U << (32 - len));
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

static Node* new_node() { return calloc(1, sizeof(Node)); }
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
static void trie_destroy(BinaryTrie* t) { trie_destroy_node(t->root); free(t); }

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
    Node* stack[33]; int path[33];
    Node* n = t->root; int depth = 0;
    for (int i = 0; i < len; i++) {
        int bit = (net >> (31 - i)) & 1;
        if (!n->child[bit]) return 0;
        stack[depth] = n;
        path[depth] = bit;
        n = n->child[bit];
        depth++;
    }
    if (!n->has_key) return 0;
    free(n->key); n->key = NULL; n->key_len = 0; n->has_key = 0;
    if (t->inserted > 0) t->inserted--;
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
        uint8_t len = rand() % 33;
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

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <n lookups per write>\n", argv[0]);
        return 1;
    }
    int n = atoi(argv[1]);
    if (n <= 0) {
        fprintf(stderr, "n must be > 0.\n");
        return 1;
    }

    // Build initial trie from prefix file
    FILE* pf = fopen(PREFIX_FILE, "r");
    if (!pf) { fprintf(stderr, "Error: cannot open %s\n", PREFIX_FILE); return 1; }
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
        if (len > 32) continue;  // Skip invalid prefix lengths
        uint32_t net; inet_pton(AF_INET, ip_part, &net);
        net = ntohl(net) & mask_from_len(len);
        unsigned char dummy_key[16] = {0};
        trie_insert(trie, net, len, dummy_key, 16);
        num_prefixes++;
    }
    fclose(pf);

    // Generate prefixes and load IPs
    size_t N = 10000000; // total operations
    PrefixRec* rand_prefixes = generate_random_prefixes(N);
    FILE* ipf = fopen(IP_FILE, "r");
    if (!ipf) { fprintf(stderr, "Error: cannot open %s\n", IP_FILE); return 1; }
    fgets(line, sizeof(line), ipf);
    size_t cap_ips = 1<<20, num_ips = 0;
    uint32_t* ips = malloc(cap_ips * sizeof(uint32_t));
    while (fgets(line, sizeof(line), ipf)) {
        char* ip_str = strtok(line, ",");
        if (!ip_str) continue;
        struct in_addr addr; inet_pton(AF_INET, ip_str, &addr);
        if (num_ips >= cap_ips) { cap_ips *= 2; ips = realloc(ips, cap_ips * sizeof(uint32_t)); }
        ips[num_ips++] = ntohl(addr.s_addr);
    }
    fclose(ipf);

    // Mixed workload timing
    uint64_t total_lookup_ns = 0, total_write_ns = 0;
    volatile int sink = 0;
    size_t write_index = 0;

    uint64_t t_start = now_ns();
    for (size_t i = 0; i < N; i++) {
        if (i % (n + 1) == 0) {
            uint64_t t0 = now_ns();
            if (write_index % 2 == 0) {
                trie_insert(trie, rand_prefixes[write_index].net,
                            rand_prefixes[write_index].len,
                            rand_prefixes[write_index].key,
                            rand_prefixes[write_index].key_len);
            } else {
                trie_delete(trie, rand_prefixes[write_index].net,
                            rand_prefixes[write_index].len);
            }
            total_write_ns += (now_ns() - t0);
            write_index++;
        } else {
            uint64_t t0 = now_ns();
            const Node* nd = trie_lpm(trie, ips[i % num_ips]);
            sink ^= (nd ? nd->key_len : 0);
            total_lookup_ns += (now_ns() - t0);
        }
    }
    uint64_t elapsed_ns = now_ns() - t_start;

    // Counts
    size_t num_writes = write_index;
    size_t num_lookups = N - num_writes;

    // Averages
    double avg_lookup_ns = (num_lookups ? (double)total_lookup_ns / num_lookups : 0.0);
    double avg_write_ns  = (num_writes  ? (double)total_write_ns  / num_writes  : 0.0);
    double avg_total_ns  = (double)elapsed_ns / N;

    printf("Ratio 1:%d -> Lookups=%zu, Writes=%zu\n", n, num_lookups, num_writes);
    printf("Avg lookup = %.2f ns, Avg write = %.2f ns, Overall = %.2f ns/op\n",
           avg_lookup_ns, avg_write_ns, avg_total_ns);

    // Write to CSV
    int need_header = !file_exists(SIM_FILE);
    FILE* f = fopen(SIM_FILE, "a");
    if (f) {
        if (need_header) {
            fprintf(f, "write_per_read_ratio,num_ops,num_lookups,num_writes,avg_lookup_ns,avg_write_ns,avg_total_ns\n");
        }
        fprintf(f, "1:%d,%zu,%zu,%zu,%.2f,%.2f,%.2f\n",
                n, N, num_lookups, num_writes,
                avg_lookup_ns, avg_write_ns, avg_total_ns);
        fclose(f);
    }

    // Cleanup
    for (size_t i = 0; i < N; i++) free(rand_prefixes[i].key);
    free(rand_prefixes); free(ips); trie_destroy(trie);
    return 0;
}
