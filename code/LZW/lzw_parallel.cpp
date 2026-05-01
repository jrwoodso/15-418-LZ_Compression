#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <chrono>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <omp.h>

/**
 * BitWriter - Writes variable-length bit codes to byte-aligned output
 * 
 * Accumulates bits in a buffer then flushes full bytes to output
 */
class BitWriter {
private:
    std::vector<uint8_t> output_bytes;
    size_t   byte_count     = 0;
    uint32_t bit_buffer     = 0;
    int      bits_in_buffer = 0;
public:
    BitWriter(size_t max_expected_size) { output_bytes.resize(max_expected_size); }
    void reset() { byte_count = 0; bit_buffer = 0; bits_in_buffer = 0; }
    void writeBits(uint32_t code, int num_bits) {
        bit_buffer |= (code << bits_in_buffer);
        bits_in_buffer += num_bits;
        while (bits_in_buffer >= 8) {
            output_bytes[byte_count++] = static_cast<uint8_t>(bit_buffer & 0xFF);
            bit_buffer >>= 8;
            bits_in_buffer -= 8;
        }
    }
    void flush() {
        if (bits_in_buffer > 0) {
            output_bytes[byte_count++] = static_cast<uint8_t>(bit_buffer & 0xFF);
            bits_in_buffer = 0; bit_buffer = 0;
        }
    }
    const uint8_t* data() const { return output_bytes.data(); }
    size_t size() const { return byte_count; }
};

/**
 * BitReader - Reads variable-length bit codes from byte-aligned input
 * 
 * Reverses the BitWriter operation, has a buffer of pending bits
 */
class BitReader {
private:
    const uint8_t* input_bytes;
    size_t   total_length;
    size_t   byte_index     = 0;
    uint32_t bit_buffer     = 0;
    int      bits_in_buffer = 0;
public:
    BitReader(const uint8_t* bytes, size_t length)
        : input_bytes(bytes), total_length(length) {}
    int readBits(int num_bits) {
        while (bits_in_buffer < num_bits) {
            if (byte_index >= total_length) {
                return -1;
            }
            bit_buffer |= (static_cast<uint32_t>(input_bytes[byte_index++]) << bits_in_buffer);
            bits_in_buffer += 8;
        }
        int code = bit_buffer & ((1U << num_bits) - 1);
        bit_buffer >>= num_bits;
        bits_in_buffer -= num_bits;
        return code;
    }
};

// LZW Configuration Constants
const int MAX_BITS      = 16;
const int MAX_DICT_SIZE = 1 << MAX_BITS;
const int      HT_BITS  = 17;
const int      HT_SIZE  = 1 << HT_BITS;
const int      HT_MASK  = HT_SIZE - 1;
const uint32_t HT_EMPTY = 0xFFFFFFFFu; 

inline uint32_t ht_hash(uint32_t key) {
    return (key * 2654435761u) >> (32 - HT_BITS);
}

// Main compression algorithm
std::vector<uint8_t> compressLZW_Block(
    const uint8_t* data, size_t length,
    uint32_t* ht_keys, uint16_t* ht_vals, uint32_t* ht_epochs, uint32_t epoch,
    BitWriter& writer)
{
    if (length == 0) {
        return {};
    }
    writer.reset();

    int nextCode  = 256;
    int bit_width = 9;
    int threshold = 512;
    uint32_t prefix_code = data[0];

    for (size_t i = 1; i < length; ++i) {
        uint32_t key = (prefix_code << 8) | data[i];
        uint32_t h   = ht_hash(key);

        if (__builtin_expect(i + 1 < length, 1)) {
            uint32_t next_key = (static_cast<uint32_t>(data[i]) << 8) | data[i + 1];
            __builtin_prefetch(ht_keys + ht_hash(next_key), 0, 1);
        }

        // Slot is only "taken" if epoch correct AND key is different
        while (ht_epochs[h] == epoch && ht_keys[h] != key)
            h = (h + 1) & HT_MASK;

        // Only valid if epoch matches
        if (ht_epochs[h] == epoch && ht_keys[h] == key) {
            prefix_code = ht_vals[h];
        } else {
            writer.writeBits(prefix_code, bit_width);
            if (nextCode < MAX_DICT_SIZE) {
                // Write new entry with current epoch
                ht_epochs[h] = epoch;
                ht_keys[h] = key;
                ht_vals[h] = static_cast<uint16_t>(nextCode);
                
                nextCode++;
                if (nextCode == threshold && bit_width < MAX_BITS) {
                    bit_width++;
                    threshold *= 2;
                }
            }
            prefix_code = data[i];
        }
    }

    writer.writeBits(prefix_code, bit_width);
    writer.flush();
    return std::vector<uint8_t>(writer.data(), writer.data() + writer.size());
}


/**
 * Arena-based dictionary for LZW decompression
 * 
 * Stores dictionary strings in a contiguous memory arena to improve
 * cache locality and reduce allocation overhead
 * 
 * Inspired by another systems class one of us took
 */
struct ArenaDict {
    std::vector<char>     arena;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> lengths;

    ArenaDict() {
        arena.reserve(128ULL * 1024 * 1024);
        offsets.reserve(MAX_DICT_SIZE);
        lengths.reserve(MAX_DICT_SIZE);
        for (int i = 0; i < 256; i++) {
            offsets.push_back(static_cast<uint32_t>(arena.size()));
            lengths.push_back(1);
            arena.push_back(static_cast<char>(i));
        }
    }

    size_t size() const { return offsets.size(); }

    std::pair<const char*, uint32_t> get(int code) const {
        return {arena.data() + offsets[code], lengths[code]};
    }

    void push(int parent_code, char suffix) {
        auto [ptr, len] = get(parent_code);
        offsets.push_back(static_cast<uint32_t>(arena.size()));
        lengths.push_back(len + 1);
        arena.insert(arena.end(), ptr, ptr + len);
        arena.push_back(suffix);
    }
};

// Main decompression algorithm
size_t decompressLZW(const uint8_t* data, size_t length,
                     char* out_buf, size_t out_buf_size)
{
    if (length == 0) return 0;

    ArenaDict dict;
    BitReader reader(data, length);
    int bit_width = 9, threshold = 512;

    int code = reader.readBits(bit_width);
    if (code == -1) return 0;

    auto [first_ptr, first_len] = dict.get(code);
    size_t out_pos = 0;
    std::memcpy(out_buf + out_pos, first_ptr, first_len);
    out_pos += first_len;
    int old_code = code;

    while ((code = reader.readBits(bit_width)) != -1) {
        const char* s_ptr; uint32_t s_len;
        bool is_new = (code == (int)dict.size());
        if (code < (int)dict.size()) {
            auto [p, l] = dict.get(code); s_ptr = p; s_len = l;
        } else if (is_new) {
            auto [p, l] = dict.get(old_code);
            dict.push(old_code, p[0]);
            auto [p2, l2] = dict.get(code); s_ptr = p2; s_len = l2;
        } else break;

        if (out_pos + s_len > out_buf_size) break;
        std::memcpy(out_buf + out_pos, s_ptr, s_len);
        out_pos += s_len;
        char suffix = s_ptr[0];
        if (!is_new && dict.size() < MAX_DICT_SIZE) dict.push(old_code, suffix);
        if ((int)dict.size() == threshold - 1 && bit_width < MAX_BITS) { bit_width++; threshold *= 2; }
        old_code = code;
    }
    return out_pos;
}

// Main loop, parses user input and compresses/decompresses
int main(int argc, char* argv[]) {
    if (argc < 4 || argc > 5) {
        std::cerr << "Usage: " << argv[0] << " [-c|-d] <input_file> <output_file> [num_threads]\n";
        return 1;
    }

    std::string mode    = argv[1];
    const char* inPath  = argv[2];
    const char* outPath = argv[3];

    int num_threads = omp_get_max_threads();
    if (argc == 5) { 
        int n = std::stoi(argv[4]); 
        if (n > 0) {
            omp_set_num_threads(n); 
            num_threads = n;
        }
    }

    int fd = open(inPath, O_RDONLY);
    if (fd == -1) { 
        std::cerr << "Error opening input.\n"; 
        return 1; 
    }

    struct stat sb;
    if (fstat(fd, &sb) == -1) { 
        close(fd); 
        return 1; 
    }

    if (sb.st_size == 0) {
        close(fd);
        int out = open(outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out != -1) {
            close(out);
        }
        return 0;
    }

    uint8_t* mapped_data = static_cast<uint8_t*>(mmap(nullptr, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (mapped_data == MAP_FAILED) { 
        std::cerr << "mmap failed.\n"; 
        close(fd); 
        return 1; 
    }
    madvise(mapped_data, sb.st_size, MADV_SEQUENTIAL);

    // Compression
    if (mode == "-c") {
        size_t total_length = sb.st_size;

        // Dynamic Block size
        size_t target_blocks = num_threads * 8; 
        size_t block_size = total_length / target_blocks;
        block_size = std::max(block_size, (size_t)(128 * 1024));   // Min 128KB
        block_size = std::min(block_size, (size_t)(2 * 1024 * 1024)); // Max 2MB

        uint32_t num_blocks = (total_length + block_size - 1) / block_size;
        std::vector<std::vector<uint8_t>> compressed_blocks(num_blocks);

        // auto t0 = std::chrono::high_resolution_clock::now();

        #pragma omp parallel
        {
            std::vector<uint32_t> ht_keys(HT_SIZE);
            std::vector<uint16_t> ht_vals(HT_SIZE);
            // Initialize epochs so epoch 0 treats as empty
            std::vector<uint32_t> ht_epochs(HT_SIZE, 0xFFFFFFFF); 
            BitWriter local_writer(block_size + block_size / 2);

            #pragma omp for schedule(dynamic, 8)
            for (size_t i = 0; i < num_blocks; ++i) {
                size_t off = i * block_size;
                size_t len = std::min(block_size, total_length - off);
                
                // Pass i as epoch
                compressed_blocks[i] = compressLZW_Block(mapped_data + off, len, ht_keys.data(), ht_vals.data(), ht_epochs.data(), static_cast<uint32_t>(i), local_writer);
            }
        }

        // std::cerr << "[Algorithm Only: " << std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count() << "s] ";

        size_t header_bytes = sizeof(uint32_t) + num_blocks * sizeof(uint32_t);
        std::vector<size_t> out_offsets(num_blocks);
        out_offsets[0] = header_bytes;
        for (size_t i = 1; i < num_blocks; ++i) {
            out_offsets[i] = out_offsets[i-1] + compressed_blocks[i-1].size();
        }
        
        size_t total_out = out_offsets[num_blocks-1] + compressed_blocks[num_blocks-1].size();

        // MMapped Output File
        int out_fd = open(outPath, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (out_fd == -1) { std::cerr << "Error opening output.\n"; return 1; }
        if (ftruncate(out_fd, static_cast<off_t>(total_out)) == -1) {
            std::cerr << "Error truncating output file.\n"; close(out_fd); return 1;
        }

        uint8_t* mapped_out = static_cast<uint8_t*>(mmap(nullptr, total_out, PROT_READ | PROT_WRITE, MAP_SHARED, out_fd, 0));
        if (mapped_out == MAP_FAILED) { 
            std::cerr << "Output mmap failed.\n"; close(out_fd); return 1; 
        }

        // Write Header
        std::memcpy(mapped_out, &num_blocks, sizeof(num_blocks));
        uint8_t* header_ptr = mapped_out + sizeof(num_blocks);
        for (size_t i = 0; i < num_blocks; ++i) {
            uint32_t b = static_cast<uint32_t>(compressed_blocks[i].size());
            std::memcpy(header_ptr, &b, sizeof(b));
            header_ptr += sizeof(b);
        }

        // Parallel Memory Copy to Disk
        #pragma omp parallel for schedule(static)
        for (size_t i = 0; i < num_blocks; ++i) {
            std::memcpy(mapped_out + out_offsets[i], compressed_blocks[i].data(), compressed_blocks[i].size());
        }

        msync(mapped_out, total_out, MS_SYNC);
        munmap(mapped_out, total_out);
        close(out_fd);

    // Decompression
    } else if (mode == "-d") {
        const uint8_t* ptr = mapped_data;

        uint32_t num_blocks;
        std::memcpy(&num_blocks, ptr, sizeof(num_blocks));
        ptr += sizeof(num_blocks);

        std::vector<uint32_t> block_sizes(num_blocks);
        std::memcpy(block_sizes.data(), ptr, num_blocks * sizeof(uint32_t));
        ptr += num_blocks * sizeof(uint32_t);

        std::vector<size_t> block_offsets(num_blocks);
        size_t cur = ptr - mapped_data;
        for (size_t i = 0; i < num_blocks; ++i) { block_offsets[i] = cur; cur += block_sizes[i]; }

        // Only allocate the outer array
        std::vector<std::vector<char>> decomp_bufs(num_blocks);
        std::vector<size_t> decomp_sizes(num_blocks, 0);

        auto t0 = std::chrono::high_resolution_clock::now();

        #pragma omp parallel
        {
            // Thread-local for locality
            const size_t SAFE_OUT_BUF = 4 * 1024 * 1024; 
            std::vector<char> local_buf(SAFE_OUT_BUF);

            #pragma omp for schedule(dynamic, 8)
            for (size_t i = 0; i < num_blocks; ++i) {
                decomp_sizes[i] = decompressLZW(mapped_data + block_offsets[i], block_sizes[i],local_buf.data(), SAFE_OUT_BUF);
                // Copy decompressed amount into new vector
                decomp_bufs[i] = std::vector<char>(local_buf.begin(), local_buf.begin() + decomp_sizes[i]);
            }
        }

        std::cerr << "[Algorithm Only: "
                  << std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count()
                  << "s] ";

        std::vector<size_t> out_offsets(num_blocks);
        out_offsets[0] = 0;
        for (size_t i = 1; i < num_blocks; ++i) {
            out_offsets[i] = out_offsets[i-1] + decomp_sizes[i-1];
        }
        size_t total_out = out_offsets[num_blocks-1] + decomp_sizes[num_blocks-1];

        int out_fd = open(outPath, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (out_fd == -1) { std::cerr << "Error opening output.\n"; return 1; }
        if (ftruncate(out_fd, static_cast<off_t>(total_out)) == -1) {
            std::cerr << "Error truncating output file.\n"; close(out_fd); return 1;
        }

        uint8_t* mapped_out = static_cast<uint8_t*>(mmap(nullptr, total_out, PROT_READ | PROT_WRITE, MAP_SHARED, out_fd, 0));
        if (mapped_out == MAP_FAILED) { std::cerr << "Output mmap failed.\n"; close(out_fd); return 1; }

        #pragma omp parallel for schedule(static)
        for (size_t i = 0; i < num_blocks; ++i) {
            if (decomp_sizes[i] == 0) {
                continue;
            }
            std::memcpy(mapped_out + out_offsets[i], decomp_bufs[i].data(), decomp_sizes[i]);
        }

        msync(mapped_out, total_out, MS_SYNC);
        munmap(mapped_out, total_out);
        close(out_fd);

    } else {
        std::cerr << "Invalid mode. Use -c or -d.\n";
    }

    munmap(mapped_data, sb.st_size);
    close(fd);
    return 0;
}
