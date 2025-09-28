// ip_lookup_cpu/src/radix_trie_api.c
#include "radix_trie_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <arpa/inet.h>  // inet_pton, AF_INET
#include <netinet/in.h> // struct in_addr

// ---- paste/move from your file: Node, BinaryTrie, PrefixRec, helpers ----
// new_node, trie_create, trie_destroy_node, trie_destroy,
// mask_from_len, hex_to_bytes, ip_str_to_uint (optional), trie_insert, trie_lpm...

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

static Node *new_node(void)
{
    return (Node *)calloc(1, sizeof(Node));
}

static BinaryTrie *trie_create(void)
{
    BinaryTrie *t = (BinaryTrie *)malloc(sizeof(BinaryTrie));
    t->root = new_node();
    t->inserted = 0;
    return t;
}

static void trie_destroy_node(Node *n)
{
    if (!n)
        return;
    trie_destroy_node(n->child[0]);
    trie_destroy_node(n->child[1]);
    if (n->has_key && n->key)
        free(n->key);
    free(n);
}

void rt_destroy(BinaryTrie *t)
{
    if (!t)
        return;
    trie_destroy_node(t->root);
    free(t);
}

static inline uint32_t mask_from_len(uint8_t len)
{
    return (len == 0) ? 0U : (~0U << (32 - len));
}

static inline unsigned char *hex_to_bytes(const char *hex, size_t *out_len)
{
    size_t len = strlen(hex) / 2;
    unsigned char *out = (unsigned char *)malloc(len);
    for (size_t i = 0; i < len; i++)
    {
        char buf[3] = {hex[2 * i], hex[2 * i + 1], 0};
        out[i] = (unsigned char)strtol(buf, NULL, 16);
    }
    *out_len = len;
    return out;
}

static void trie_insert(BinaryTrie *t, uint32_t net, uint8_t len,
                        unsigned char *key, size_t key_len)
{
    if (len > 0)
        net &= mask_from_len(len);
    Node *n = t->root;
    for (int i = 0; i < len; i++)
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

static const Node *trie_lpm(const BinaryTrie *t, uint32_t ip)
{
    const Node *best = NULL;
    const Node *n = t->root;
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

// ---- public API ----
BinaryTrie *rt_load_csv(const char *prefix_csv_path)
{
    FILE *pf = fopen(prefix_csv_path, "r");
    if (!pf)
        return NULL;

    BinaryTrie *trie = trie_create();
    char line[8192];
    int first = 1;

    while (fgets(line, sizeof(line), pf))
    {
        char *s = line;
        while (*s == ' ' || *s == '\t')
            ++s; // trim leading spaces

        if (first)
        {
            first = 0;
            if (!(*s >= '0' && *s <= '9'))
            {
                // header line → skip it
                continue;
            }
        }

        char *comma = strchr(s, ',');
        if (!comma)
            continue;
        *comma = '\0';
        char *prefix_str = s;
        char *key_hex = comma + 1;

        // strip trailing \r\n and spaces from key_hex
        char *end = key_hex + strlen(key_hex);
        while (end > key_hex && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';

        // prefix: a.b.c.d/len
        char *slash = strchr(prefix_str, '/');
        if (!slash)
            continue;
        *slash = '\0';
        const char *ip_s = prefix_str;
        // strip trailing spaces on ip part
        char *ip_end = (char *)ip_s + strlen(ip_s);
        while (ip_end > ip_s && (ip_end[-1] == ' ' || ip_end[-1] == '\t'))
            *--ip_end = '\0';

        uint8_t len = (uint8_t)atoi(slash + 1);
        struct in_addr addr;
        if (inet_pton(AF_INET, ip_s, &addr) != 1)
            continue;
        uint32_t net = ntohl(addr.s_addr) & mask_from_len(len);

        size_t key_len = 0;
        unsigned char *key = hex_to_bytes(key_hex, &key_len);
        if (!key || key_len == 0)
            continue;

        trie_insert(trie, net, len, key, key_len);
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