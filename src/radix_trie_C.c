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

static void trim_eol(char *s)
{
    if (!s)
        return;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        --e;
    *e = '\0';
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