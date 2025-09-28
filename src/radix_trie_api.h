// ip_lookup_cpu/src/radix_trie_api.h
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BinaryTrie BinaryTrie;

// Load "prefix,keyhex" CSV into a trie.
// Returns NULL on error. Keys are stored as raw bytes.
BinaryTrie* rt_load_csv(const char* prefix_csv_path);

// Look up longest prefix match for IPv4 address (host byte order).
// Returns 1 if found and sets *key/*key_len to point into the trie's storage
// (do NOT free). Returns 0 if no match.
int rt_lookup_key(const BinaryTrie* t, uint32_t ip_hbo,
                  const unsigned char** key, size_t* key_len);

// Free the trie and all key storage.
void rt_destroy(BinaryTrie* t);

#ifdef __cplusplus
}
#endif