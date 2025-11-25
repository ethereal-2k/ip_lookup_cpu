// ip_lookup_cpu/src/radix_trie_api.c
#include "radix_trie_api.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ----------------- internal trie types -----------------
typedef struct Node
{
    struct Node *child[2];
    int has_key;
    unsigned char *key;
    size_t key_len;
} Node;

struct BinaryTrie
{
    Node *root;
    size_t inserted;
};

// ----------------- helpers -----------------
static Node *new_node(void) { return (Node *)calloc(1, sizeof(Node)); }

static inline uint32_t mask_from_len(uint8_t len)
{
    return (len == 0) ? 0U : (~0U << (32 - len));
}

static inline unsigned char *hex_to_bytes(const char *hex, size_t *out_len)
{
    size_t n = strlen(hex);
    if (n & 1)
        return NULL;
    size_t len = n / 2;
    unsigned char *out = (unsigned char *)malloc(len);
    if (!out)
        return NULL;
    for (size_t i = 0; i < len; i++)
    {
        char buf[3] = {hex[2 * i], hex[2 * i + 1], 0};
        out[i] = (unsigned char)strtol(buf, NULL, 16);
    }
    *out_len = len;
    return out;
}

static void trie_insert(struct BinaryTrie *t, uint32_t net, uint8_t len,
                        unsigned char *key, size_t key_len)
{
    if (len > 0)
        net &= mask_from_len(len);
    Node *n = t->root;
    for (int i = 0; i < (int)len; i++)
    {
        int bit = (net >> (31 - i)) & 1;
        if (!n->child[bit])
            n->child[bit] = new_node();
        n = n->child[bit];
    }
    if (n->has_key && n->key)
        free(n->key);
    n->has_key = 1;
    n->key = key;
    n->key_len = key_len;
    t->inserted++;
}

static const Node *trie_lpm(const struct BinaryTrie *t, uint32_t ip)
{
    const Node *best = NULL, *n = t->root;
    if (n->has_key)
        best = n;
    for (int i = 0; i < 32 && n; i++)
    {
        int bit = (ip >> (31 - i)) & 1;
        n = n->child[bit];
        if (!n)
            break;
        if (n->has_key)
            best = n;
    }
    return best;
}

<<<<<<< HEAD
static void trim_eol(char *s)
{
    if (!s)
        return;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        --e;
    *e = '\0';
=======
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
        // Remove trailing newline
        size_t len_line = strlen(line);
        if (len_line > 0 && line[len_line-1] == '\n') {
            line[len_line-1] = '\0';
        }
        char* prefix_str = strtok(line, ",");
        char* key_hex = strtok(NULL, ",");
        if (!prefix_str || !key_hex) continue;
        char* slash = strchr(prefix_str, '/');
        if (!slash) continue;

        *slash = '\0';
        char* ip_part = prefix_str;
        uint8_t len = (uint8_t)atoi(slash+1);
        if (len > 32) continue;  // Skip invalid prefix lengths
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

    // Free keys before freeing prefix array
    for (size_t i = 0; i < num_prefixes; i++) {
        if (prefixes[i].key) free(prefixes[i].key);
    }
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
        // Remove trailing newline
        size_t len_line = strlen(line);
        if (len_line > 0 && line[len_line-1] == '\n') {
            line[len_line-1] = '\0';
        }
        char* ip_str = strtok(line, ",");
        strtok(NULL, ",");  // Skip second column
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
>>>>>>> 5882c1b (added workload analysis files and modified existing files to fix minor bugs)
}

// ----------------- public API -----------------
BinaryTrie *rt_load_csv(const char *prefix_csv_path)
{
    FILE *pf = fopen(prefix_csv_path, "r");
    if (!pf)
        return NULL;

    BinaryTrie *trie = (BinaryTrie *)malloc(sizeof(BinaryTrie));
    if (!trie)
    {
        fclose(pf);
        return NULL;
    }
    trie->root = new_node();
    trie->inserted = 0;

    char line[8192];
    int first = 1;

    while (fgets(line, sizeof(line), pf))
    {
        trim_eol(line);
        if (!*line)
            continue;

        const char *p = line;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (!*p || *p == '#')
            continue;

        // Header detection on first non-empty line
        if (first)
        {
            first = 0;
            if (!(isdigit((unsigned char)*p)))
                continue; // header → skip
        }

        char *comma = strchr(line, ',');
        if (!comma)
            continue;
        *comma = '\0';
        char *prefix_str = line;
        char *key_str = comma + 1;
        trim_eol(prefix_str);
        trim_eol(key_str);

        // a.b.c.d/len
        char *slash = strchr(prefix_str, '/');
        if (!slash)
            continue;
        *slash = '\0';
        const char *ip_s = prefix_str;
        int plen = atoi(slash + 1);
        if (plen < 0 || plen > 32)
            continue;

        struct in_addr addr;
        if (inet_pton(AF_INET, ip_s, &addr) != 1)
            continue;
        uint32_t net = ntohl(addr.s_addr) & mask_from_len((uint8_t)plen);

        // --- KEY: hex if 128 chars, else raw bytes ---
        size_t key_len = 0;
        unsigned char *key = NULL;
        size_t klen = strlen(key_str);
        if (klen == 128)
        {
            key = hex_to_bytes(key_str, &key_len);
            if (!key)
                continue;
        }
        else
        {
            key_len = klen;
            key = (unsigned char *)malloc(key_len);
            if (!key)
                continue;
            memcpy(key, key_str, key_len);
        }

        trie_insert(trie, net, (uint8_t)plen, key, key_len);
    }

    fclose(pf);
    return trie;
}

int rt_lookup_key(const BinaryTrie *t, uint32_t ip_hbo,
                  const unsigned char **key, size_t *key_len)
{
    if (!t)
        return 0;
    const Node *n = trie_lpm(t, ip_hbo);
    if (!n)
        return 0;
    if (key)
        *key = n->key;
    if (key_len)
        *key_len = n->key_len;
    return 1;
}

void rt_destroy(BinaryTrie *t)
{
    if (!t)
        return;
    // iterative free to avoid deep recursion
    Node *stack[4096];
    int sp = 0;
    if (t->root)
        stack[sp++] = t->root;
    while (sp)
    {
        Node *n = stack[--sp];
        if (n->child[0])
            stack[sp++] = n->child[0];
        if (n->child[1])
            stack[sp++] = n->child[1];
        if (n->has_key && n->key)
            free(n->key);
        free(n);
    }
    free(t);
}