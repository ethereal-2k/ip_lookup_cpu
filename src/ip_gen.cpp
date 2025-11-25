#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <random>
#include <iomanip>
#include <unordered_set>
#include <arpa/inet.h>
#include <utility>
#include <cstdlib>

uint32_t ip_str_to_uint(const std::string& ip_str) {
    in_addr addr;
    inet_pton(AF_INET, ip_str.c_str(), &addr);
    return ntohl(addr.s_addr);
}

std::string uint_to_ip_str(uint32_t ip) {
    in_addr addr;
    addr.s_addr = htonl(ip);
    char ip_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, ip_buf, INET_ADDRSTRLEN);
    return std::string(ip_buf);
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <num_ips_to_generate>\n";
        return 1;
    }

    int N = std::stoi(argv[1]);
    if (N <= 0) {
        std::cerr << "Number of IPs must be positive.\n";
        return 1;
    }

    // Input: prefix table in data/
    std::ifstream file("data/prefix_table.csv");
    if (!file.is_open()) {
        std::cerr << "Error: Could not open data/prefix_table.csv\n";
        return 1;
    }

    std::string line;
    std::getline(file, line); // skip header

    std::vector<std::pair<uint32_t, uint8_t>> prefixes;
    std::vector<std::string> prefix_strings;

    while (std::getline(file, line)) {
        std::istringstream ss(line);
        std::string prefix_str, key_hex;
        if (!std::getline(ss, prefix_str, ',') || !std::getline(ss, key_hex)) continue;

        auto slash_pos = prefix_str.find('/');
        std::string ip_str = prefix_str.substr(0, slash_pos);
        uint8_t prefix_len = std::stoi(prefix_str.substr(slash_pos + 1));
        uint32_t ip = ip_str_to_uint(ip_str);
        uint32_t mask = prefix_len == 0 ? 0 : (~0U << (32 - prefix_len));
        ip = ip & mask;  // align to prefix boundary

        prefixes.emplace_back(ip, prefix_len);
        prefix_strings.push_back(prefix_str);
    }

    // Output: generated IPs in data/
    std::ofstream out("data/generated_ips.csv");
    if (!out.is_open()) {
        std::cerr << "Error: Could not open data/generated_ips.csv for writing\n";
        return 1;
    }

    out << "ip,used_prefix\n";

    std::random_device rd;
    std::mt19937 gen(rd());
    std::unordered_set<uint32_t> generated_ips;

    int count = 0;
    while (count < N) {
        std::uniform_int_distribution<size_t> prefix_dist(0, prefixes.size() - 1);
        size_t idx = prefix_dist(gen);

        uint32_t base_ip = prefixes[idx].first;
        uint8_t len = prefixes[idx].second;
        uint32_t host_bits = 32 - len;
        uint32_t max_suffix = (host_bits == 0) ? 0 : (1U << host_bits) - 1;

        std::uniform_int_distribution<uint32_t> dist(0, max_suffix);
        uint32_t suffix = dist(gen);
        uint32_t ip = base_ip | suffix;

        if (generated_ips.insert(ip).second) {
            out << uint_to_ip_str(ip) << "," << prefix_strings[idx] << "\n";
            ++count;
        }
    }

    std::cout << "Generated " << N << " unique IPs using prefixes from ../data/prefix_table.csv\n";
    return 0;
}
