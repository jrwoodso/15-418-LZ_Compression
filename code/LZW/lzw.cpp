#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <chrono>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// BIT PACKING CLASSES (for formatting input/output)
class BitWriter {
private:
    std::vector<uint8_t> output_bytes;
    uint32_t bit_buffer = 0;   
    int bits_in_buffer = 0;    

public:
    // Pre-allocate memory for compression
    BitWriter(size_t expected_size) {
        output_bytes.reserve(expected_size);
    }

    void writeBits(uint32_t code, int num_bits) {
        bit_buffer |= (code << bits_in_buffer);
        bits_in_buffer += num_bits;

        while (bits_in_buffer >= 8) {
            output_bytes.push_back(static_cast<uint8_t>(bit_buffer & 0xFF));
            bit_buffer >>= 8;                          
            bits_in_buffer -= 8;                       
        }
    }

    void flush() {
        if (bits_in_buffer > 0) {
            output_bytes.push_back(static_cast<uint8_t>(bit_buffer & 0xFF));
            bits_in_buffer = 0;
            bit_buffer = 0;
        }
    }

    const std::vector<uint8_t>& getBytes() const {
        return output_bytes;
    }
};

class BitReader {
private:
    const uint8_t* input_bytes;
    size_t total_length;
    size_t byte_index = 0;
    uint32_t bit_buffer = 0;
    int bits_in_buffer = 0;

public:
    BitReader(const uint8_t* bytes, size_t length) : input_bytes(bytes), total_length(length) {}

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

// CORE ALGORITHMS ---

const int MAX_BITS = 16;
const int MAX_DICT_SIZE = 1 << MAX_BITS; 

// Use raw pointer and length for Zero-Copy I/O
std::vector<uint8_t> compressLZW(const uint8_t* data, size_t length) {
    if (length == 0) return { };

    std::vector<int> dict(1 << 24, -1); 
    
    // Estimate output size to prevent vector re-allocations
    BitWriter writer(length / 2); 
    
    int nextCode = 256;
    int bit_width = 9;
    int threshold = 512; 

    uint32_t prefix_code = data[0];

    for (size_t i = 1; i < length; ++i) {
        unsigned char c = data[i];
        uint32_t current_key = (prefix_code << 8) | c;

        if (dict[current_key] != -1) {
            prefix_code = dict[current_key];
        } else {
            writer.writeBits(prefix_code, bit_width);
            
            if (nextCode < MAX_DICT_SIZE) {
                dict[current_key] = nextCode++;
                if (nextCode == threshold && bit_width < MAX_BITS) {
                    bit_width++;
                    threshold *= 2;
                }
            }
            prefix_code = c;
        }
    }

    writer.writeBits(prefix_code, bit_width);
    writer.flush();
    return writer.getBytes();
}

std::string decompressLZW(const uint8_t* data, size_t length) {
    if (length == 0) return "";

    std::vector<std::string> dict(256);
    for (int i = 0; i < 256; i++) {
        dict[i] = std::string(1, static_cast<char>(i));
    }

    BitReader reader(data, length);
    int bit_width = 9;
    int threshold = 512;
    
    int code = reader.readBits(bit_width);
    if (code == -1) return "";

    std::string old = dict[code];
    std::string result = old;
    // Pre-allocate to prevent continuous string re-allocations during append
    result.reserve(length * 3); 
    
    char c = old[0];

    while ((code = reader.readBits(bit_width)) != -1) {
        std::string s;
        if (code < static_cast<int>(dict.size())) {
            s = dict[code];
        } else if (code == static_cast<int>(dict.size())) {
            s = old + c;
        } else {
            break; 
        }

        result += s;
        c = s[0];

        if (static_cast<int>(dict.size()) < MAX_DICT_SIZE) {
            dict.push_back(old + c);
            if (static_cast<int>(dict.size()) == threshold - 1 && bit_width < MAX_BITS) {
                bit_width++;
                threshold *= 2;
            }
        }
        old = s;
    }

    return result;
}

// MAIN FUNCTION -- allow for user input
int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " [-c|-d] <input_file> <output_file>\n";
        return 1;
    }

    std::string mode = argv[1];
    const char* inPath = argv[2];
    const char* outPath = argv[3];

    // 1. Open File
    int fd = open(inPath, O_RDONLY);
    if (fd == -1) {
        std::cerr << "Error opening file.\n";
        return 1;
    }

    // 2. Get File Size
    struct stat sb;
    if (fstat(fd, &sb) == -1) {
        close(fd);
        return 1;
    }

    if (sb.st_size == 0) {
        close(fd);
        open(outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644); // Create empty file
        return 0;
    }

    // 3. MMAP: Map the file directly into RAM
    uint8_t* mapped_data = static_cast<uint8_t*>(mmap(nullptr, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (mapped_data == MAP_FAILED) {
        std::cerr << "mmap failed.\n";
        close(fd);
        return 1;
    }

    // 4. Process the data
    if (mode == "-c") {
        // auto start = std::chrono::high_resolution_clock::now();

        std::vector<uint8_t> compressed = compressLZW(mapped_data, sb.st_size);

        // auto end = std::chrono::high_resolution_clock::now();
        // std::chrono::duration<double> diff = end - start;
        // std::cerr << "[Alg Only: " << diff.count() << "s] "; // Silence internal timing
        
        // Write Output via raw POSIX write
        int out_fd = open(outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ssize_t bytes_written = write(out_fd, compressed.data(), compressed.size());
        if (bytes_written == -1 || static_cast<size_t>(bytes_written) != compressed.size()) {
            std::cerr << "Error: Failed to write all compressed bytes to disk.\n";
        }
        close(out_fd);

    } else if (mode == "-d") {
        std::string decompressed = decompressLZW(mapped_data, sb.st_size);
        
        int out_fd = open(outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ssize_t bytes_written = write(out_fd, decompressed.data(), decompressed.size());
        if (bytes_written == -1 || static_cast<size_t>(bytes_written) != decompressed.size()) {
            std::cerr << "Error: Failed to write all decompressed bytes to disk.\n";
        }
        close(out_fd);
    } else {
        std::cerr << "Invalid mode.\n";
    }

    // 5. Cleanup
    munmap(mapped_data, sb.st_size);
    close(fd);

    return 0;
}
