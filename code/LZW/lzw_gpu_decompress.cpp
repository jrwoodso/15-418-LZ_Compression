#include <iostream>
#include <fstream>
#include <vector>
#include <string>

// Simple LZW decompressor for fixed-size image (12-bit codes)
int main(int argc, char* argv[]) {
    if (argc < 3) {
        return 1;
    }

    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    if (!in) {
        return 1;
    }

    std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);

    std::vector<unsigned char> raw_bytes(size);
    in.read(reinterpret_cast<char*>(raw_bytes.data()), size);

    std::ofstream out(argv[2], std::ios::binary);
    if (raw_bytes.empty()) {
        return 0;
    }

    size_t byte_idx = 0;
    unsigned int bit_buffer = 0;
    int bit_count = 0;

    // Helper: reads next 12-bit LZW code from byte stream
    auto read_code = [&]() -> unsigned short {
        while (bit_count < 12 && byte_idx < raw_bytes.size()) {
            bit_buffer |= (static_cast<unsigned int>(raw_bytes[byte_idx++]) << bit_count);
            bit_count += 8;
        }
        if (bit_count < 12) {
            return 257; 
        }
        
        unsigned short code = bit_buffer & 0xFFF; 
        bit_buffer >>= 12;
        bit_count -= 12;
        return code;
    };

    std::vector<std::string> dictionary(4096);
    for (int i = 0; i < 256; i++) {
        dictionary[i] = std::string(1, static_cast<char>(i));
    }

    int width = 4096, height = 3072;
    std::vector<unsigned char> image_buffer(width * height);
    size_t buffer_offset = 0;

    // Process image strip-by-strip (one strip per row)
    for (int strip = 0; strip < height; strip++) {
        int dict_size = 258;
        unsigned short old_code = read_code();

        if (old_code == 257) {
            bit_buffer = 0; bit_count = 0;
            continue;
        }

        std::string s = dictionary[old_code];
        for (char c : s) {
            if (buffer_offset < image_buffer.size()) {
                image_buffer[buffer_offset++] = c;
            }
        }

        while (true) {
            unsigned short new_code = read_code();

            if (new_code == 257) { 
                bit_buffer = 0; bit_count = 0;
                break;
            }

            if (new_code == 256) { 
                dict_size = 258;
                old_code = read_code();
                if (old_code == 257) break; 
                
                s = dictionary[old_code];
                for (char c : s) {
                    if (buffer_offset < image_buffer.size()) {
                        image_buffer[buffer_offset++] = c;
                    }
                }
                continue;
            }

            std::string entry;
            if (new_code < dict_size) {
                entry = dictionary[new_code];
            } else if (new_code == dict_size) {
                entry = dictionary[old_code] + dictionary[old_code][0];
            } else {
                std::cerr << "\n BAD!!! Decompression error!\n";
                return 1;
            }

            for (char c : entry) {
                if (buffer_offset < image_buffer.size()) image_buffer[buffer_offset++] = c;
            }

            if (dict_size < 4094) {
                dictionary[dict_size++] = dictionary[old_code] + entry[0];
            }
            old_code = new_code;
        }
    }

    // Write output
    out.write(reinterpret_cast<char*>(image_buffer.data()), image_buffer.size());
    std::cout << "\n Decompressed " << height << " strips.\n";

    return 0;
}
