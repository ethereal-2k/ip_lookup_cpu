#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <time.h>

// Struct for a FIB entry
typedef struct {
    uint32_t network_prefix;
    uint8_t prefix_len;
    char prefix_str[32];
    char key_hex[129]; // 64 bytes = 128 hex chars + null
} FibEntry;

// Convert prefix to string form (ip/prefix_len)
void ip_prefix_to_string(uint32_t network_prefix, uint8_t prefix_len, char *out) {
    struct in_addr addr;
    addr.s_addr = htonl(network_prefix);
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, ip_str, INET_ADDRSTRLEN);
    sprintf(out, "%s/%u", ip_str, prefix_len);
}

// Convert 64-byte key into hex string
void bytes_to_hex(uint8_t *bytes, size_t len, char *out) {
    for (size_t i = 0; i < len; i++) {
        sprintf(out + i*2, "%02x", bytes[i]);
    }
    out[len*2] = '\0';
}

// Comparator for sorting prefixes by prefix_len (descending)
int cmp_prefix_len(const void *a, const void *b) {
    const FibEntry *pa = (const FibEntry *)a;
    const FibEntry *pb = (const FibEntry *)b;
    return (pb->prefix_len - pa->prefix_len);
}

int main(int argc, char *argv[]) {
    int N = 10000; // default
    if (argc >= 2) {
        N = atoi(argv[1]);
        if (N <= 0) {
            fprintf(stderr, "Number of prefixes must be > 0.\n");
            return 1;
        }
    }

    srand((unsigned)time(NULL));

    FibEntry *entries = malloc(N * sizeof(FibEntry));
    if (!entries) {
        fprintf(stderr, "Memory allocation failed.\n");
        return 1;
    }

    int count = 0;
    while (count < N) {
        uint8_t prefix_len = (rand() % 25) + 8; // random [8,32]
        uint32_t ip = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
        uint32_t mask = prefix_len == 0 ? 0 : (~0U << (32 - prefix_len));
        uint32_t network_prefix = ip & mask;

        // Convert to string and ensure uniqueness (check manually)
        char prefix_str[32];
        ip_prefix_to_string(network_prefix, prefix_len, prefix_str);

        int duplicate = 0;
        for (int i = 0; i < count; i++) {
            if (strcmp(entries[i].prefix_str, prefix_str) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) continue;

        // Fill entry
        entries[count].network_prefix = network_prefix;
        entries[count].prefix_len = prefix_len;
        strcpy(entries[count].prefix_str, prefix_str);

        // Generate random 64-byte key
        uint8_t key[64];
        for (int j = 0; j < 64; j++) key[j] = rand() & 0xFF;
        bytes_to_hex(key, 64, entries[count].key_hex);

        count++;
    }

    // Sort by prefix_len descending
    qsort(entries, N, sizeof(FibEntry), cmp_prefix_len);

    // Write to CSV
    FILE *fout = fopen("./data/prefix_table.csv", "w");
    if (!fout) {
        fprintf(stderr, "Error opening output file.\n");
        free(entries);
        return 1;
    }
    fprintf(fout, "prefix,key\n");
    for (int i = 0; i < N; i++) {
        fprintf(fout, "%s,%s\n", entries[i].prefix_str, entries[i].key_hex);
    }
    fclose(fout);

    printf("Generated sorted prefix_table.csv with %d unique, aligned prefixes.\n", N);

    free(entries);
    return 0;
}
