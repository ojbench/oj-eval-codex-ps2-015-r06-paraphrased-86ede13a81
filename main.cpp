#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr const char *kDataFileName = "storage.db";
constexpr uint32_t kMagic = 0x53444231u;
constexpr uint32_t kVersion = 1u;
constexpr uint32_t kBucketCount = 262139u;
constexpr int32_t kEmpty = -1;

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t bucket_count;
    int32_t node_count;
    int32_t free_head;
};

struct Node {
    int32_t next;
    int32_t value;
    uint8_t key_len;
    char key[64];
};
#pragma pack(pop)

static_assert(sizeof(Node) == 73, "Unexpected node size");

class Database {
public:
    Database() : file_(nullptr) {}

    ~Database() {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    bool open() {
        file_ = std::fopen(kDataFileName, "r+b");
        if (file_ == nullptr) {
            file_ = std::fopen(kDataFileName, "w+b");
            if (file_ == nullptr) {
                return false;
            }
            Header header{};
            header.magic = kMagic;
            header.version = kVersion;
            header.bucket_count = kBucketCount;
            header.node_count = 0;
            header.free_head = kEmpty;
            if (!write_header(header)) {
                return false;
            }
            std::vector<int32_t> buckets(kBucketCount, kEmpty);
            if (!seek_to(bucket_offset(0)) || std::fwrite(buckets.data(), sizeof(int32_t), buckets.size(), file_) != buckets.size()) {
                return false;
            }
            std::fflush(file_);
        }
        return read_header(header_) && header_.magic == kMagic && header_.version == kVersion && header_.bucket_count == kBucketCount;
    }

    void insert(const std::string &key, int32_t value) {
        uint32_t bucket = hash_key(key) % kBucketCount;
        int32_t head = read_bucket(bucket);
        int32_t current = head;
        Node node{};
        while (current != kEmpty) {
            if (!read_node(current, node)) {
                return;
            }
            if (same_key(node, key) && node.value == value) {
                return;
            }
            current = node.next;
        }

        int32_t index = allocate_node();
        if (index == kEmpty) {
            return;
        }
        Node fresh{};
        fresh.next = head;
        fresh.value = value;
        fresh.key_len = static_cast<uint8_t>(key.size());
        std::memcpy(fresh.key, key.data(), key.size());
        if (!write_node(index, fresh)) {
            return;
        }
        write_bucket(bucket, index);
    }

    void erase(const std::string &key, int32_t value) {
        uint32_t bucket = hash_key(key) % kBucketCount;
        int32_t current = read_bucket(bucket);
        int32_t previous = kEmpty;
        Node node{};
        while (current != kEmpty) {
            if (!read_node(current, node)) {
                return;
            }
            if (same_key(node, key) && node.value == value) {
                if (previous == kEmpty) {
                    write_bucket(bucket, node.next);
                } else {
                    Node prev_node{};
                    if (!read_node(previous, prev_node)) {
                        return;
                    }
                    prev_node.next = node.next;
                    if (!write_node(previous, prev_node)) {
                        return;
                    }
                }
                node.next = header_.free_head;
                if (!write_node(current, node)) {
                    return;
                }
                header_.free_head = current;
                write_header(header_);
                return;
            }
            previous = current;
            current = node.next;
        }
    }

    void find(const std::string &key) {
        uint32_t bucket = hash_key(key) % kBucketCount;
        int32_t current = read_bucket(bucket);
        std::vector<int32_t> values;
        Node node{};
        while (current != kEmpty) {
            if (!read_node(current, node)) {
                return;
            }
            if (same_key(node, key)) {
                values.push_back(node.value);
            }
            current = node.next;
        }
        if (values.empty()) {
            std::cout << "null\n";
            return;
        }
        std::sort(values.begin(), values.end());
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                std::cout << ' ';
            }
            std::cout << values[i];
        }
        std::cout << '\n';
    }

private:
    FILE *file_;
    Header header_{};

    static uint64_t bucket_offset(uint32_t bucket) {
        return sizeof(Header) + static_cast<uint64_t>(bucket) * sizeof(int32_t);
    }

    static uint64_t node_offset(int32_t index) {
        return sizeof(Header) + static_cast<uint64_t>(kBucketCount) * sizeof(int32_t) + static_cast<uint64_t>(index) * sizeof(Node);
    }

    static uint32_t hash_key(const std::string &key) {
        uint64_t hash = 1469598103934665603ull;
        for (unsigned char ch : key) {
            hash ^= ch;
            hash *= 1099511628211ull;
        }
        return static_cast<uint32_t>((hash >> 32) ^ (hash & 0xffffffffu));
    }

    static bool same_key(const Node &node, const std::string &key) {
        return node.key_len == key.size() && std::memcmp(node.key, key.data(), key.size()) == 0;
    }

    bool seek_to(uint64_t offset) {
        return std::fseek(file_, static_cast<long>(offset), SEEK_SET) == 0;
    }

    bool read_header(Header &header) {
        if (!seek_to(0)) {
            return false;
        }
        return std::fread(&header, sizeof(Header), 1, file_) == 1;
    }

    bool write_header(const Header &header) {
        if (!seek_to(0)) {
            return false;
        }
        if (std::fwrite(&header, sizeof(Header), 1, file_) != 1) {
            return false;
        }
        std::fflush(file_);
        return true;
    }

    int32_t read_bucket(uint32_t bucket) {
        int32_t value = kEmpty;
        if (!seek_to(bucket_offset(bucket))) {
            return kEmpty;
        }
        if (std::fread(&value, sizeof(int32_t), 1, file_) != 1) {
            return kEmpty;
        }
        return value;
    }

    bool write_bucket(uint32_t bucket, int32_t value) {
        if (!seek_to(bucket_offset(bucket))) {
            return false;
        }
        if (std::fwrite(&value, sizeof(int32_t), 1, file_) != 1) {
            return false;
        }
        std::fflush(file_);
        return true;
    }

    bool read_node(int32_t index, Node &node) {
        if (!seek_to(node_offset(index))) {
            return false;
        }
        return std::fread(&node, sizeof(Node), 1, file_) == 1;
    }

    bool write_node(int32_t index, const Node &node) {
        if (!seek_to(node_offset(index))) {
            return false;
        }
        if (std::fwrite(&node, sizeof(Node), 1, file_) != 1) {
            return false;
        }
        std::fflush(file_);
        return true;
    }

    int32_t allocate_node() {
        if (header_.free_head != kEmpty) {
            int32_t index = header_.free_head;
            Node node{};
            if (!read_node(index, node)) {
                return kEmpty;
            }
            header_.free_head = node.next;
            if (!write_header(header_)) {
                return kEmpty;
            }
            return index;
        }
        int32_t index = header_.node_count;
        ++header_.node_count;
        if (!write_header(header_)) {
            return kEmpty;
        }
        return index;
    }
};

}  // namespace

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    Database database;
    if (!database.open()) {
        return 1;
    }

    int n = 0;
    std::cin >> n;
    std::string command;
    std::string key;
    int32_t value = 0;
    for (int i = 0; i < n; ++i) {
        std::cin >> command >> key;
        if (command[0] == 'f') {
            database.find(key);
        } else {
            std::cin >> value;
            if (command[0] == 'i') {
                database.insert(key, value);
            } else {
                database.erase(key, value);
            }
        }
    }
    return 0;
}
