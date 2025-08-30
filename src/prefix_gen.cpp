#include <iostream>
#include <fstream>
#include <vector>
#include <unordered_set>
#include <random>
#include <string>
#include <sstream>
#include <iomanip>
#include <arpa/inet.h>
#include <algorithm>

std::string ip_prefix_to_string(uint32_t network_prefix, uint8_t prefix_len) {
    in_addr addr;
    addr.s_addr = htonl(network_prefix);  // Use aligned IP
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, ip_str, INET_ADDRSTRLEN);
    return std::string(ip_str) + "/" + std::to_string(prefix_len);
}

std::string bytes_to_hex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    for (auto b : bytes)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    return oss.str();
}

int main(int argc, char* argv[]) {
    // Default: 10000 prefixes if no argument provided
    int N = 10000;
    if (argc >= 2) {
        try {
            N = std::stoi(argv[1]);
            if (N <= 0) {
                std::cerr << "Number of prefixes must be > 0.\n";
                return 1;
            }
        } catch (...) {
            std::cerr << "Invalid number format for prefixes.\n";
            return 1;
        }
    }

    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<uint32_t> ip_dist(0, 0xFFFFFFFF);
    std::uniform_int_distribution<int> len_dist(8, 32);
    std::uniform_int_distribution<uint8_t> byte_dist(0, 255);

    std::unordered_set<std::string> prefix_set;

    struct FibEntry {
        std::string prefix_str;
        std::string key_hex;
        uint8_t prefix_len;
    };

    std::vector<FibEntry> entries;

    int count = 0;
    while (count < N) {
        uint8_t prefix_len = len_dist(rng);
        uint32_t ip = ip_dist(rng);
        uint32_t mask = prefix_len == 0 ? 0 : (~0U << (32 - prefix_len));
        uint32_t network_prefix = ip & mask;

        std::string prefix_str = ip_prefix_to_string(network_prefix, prefix_len);
        if (prefix_set.find(prefix_str) != prefix_set.end())
            continue;

        prefix_set.insert(prefix_str);

        std::vector<uint8_t> key(64);
        for (auto& b : key)
            b = byte_dist(rng);

        entries.push_back({prefix_str, bytes_to_hex(key), prefix_len});
        count++;
    }

    // Sort by prefix_len descending
    std::sort(entries.begin(), entries.end(),
              [](const FibEntry& a, const FibEntry& b) {
                  return a.prefix_len > b.prefix_len;
              });

    std::ofstream fout("../data/prefix_table.csv");
    fout << "prefix,key\n";
    for (const auto& entry : entries)
        fout << entry.prefix_str << "," << entry.key_hex << "\n";

    std::cout << "Generated sorted fib_table.csv with " << N << " unique, aligned prefixes.\n";
    return 0;
}