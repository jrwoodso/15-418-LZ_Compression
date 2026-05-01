#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <omp.h>

namespace fs = std::filesystem;

const size_t BLOCK_SIZE = 1024 * 1024;

/* 
 * Note: uses many of the same classes as lzw_parallel.cpp
 * including BitWriter, BitReader, ArenaDict
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
            if (byte_index >= total_length) return -1;
            bit_buffer |= (static_cast<uint32_t>(input_bytes[byte_index++]) << bits_in_buffer);
            bits_in_buffer += 8;
        }
        int code = bit_buffer & ((1U << num_bits) - 1);
        bit_buffer >>= num_bits;
        bits_in_buffer -= num_bits;
        return code;
    }
};

// File parallel LZW parameters
const int MAX_BITS      = 16;
const int MAX_DICT_SIZE = 1 << MAX_BITS;
const int      HT_BITS  = 17;
const int      HT_SIZE  = 1 << HT_BITS;
const int      HT_MASK  = HT_SIZE - 1;
const uint32_t HT_EMPTY = 0xFFFFFFFFu; // Special value

inline uint32_t ht_hash(uint32_t key) {
    return (key * 2654435761u) >> (32 - HT_BITS);
}

// Main compression block (same as lzw_parallel.cpp)
std::vector<uint8_t> compressLZW_Block(
    const uint8_t* data, size_t length,
    uint32_t* ht_keys, uint16_t* ht_vals,
    BitWriter& writer)
{
    if (length == 0) {
        return {};
    }
    writer.reset();

    int nextCode = 256;
    int bit_width = 9;
    int threshold = 512;
    uint32_t prefix_code = data[0];

    for (size_t i = 1; i < length; ++i) {
        uint32_t key = (prefix_code << 8) | data[i];
        uint32_t h = ht_hash(key);

        // Prefetch the next lookup assuming miss
        // Inspired by lecture concepts
        if (__builtin_expect(i + 1 < length, 1)) {
            uint32_t next_key = (static_cast<uint32_t>(data[i]) << 8) | data[i + 1];
            __builtin_prefetch(ht_keys + ht_hash(next_key), 0, 1);
        }

        while (ht_keys[h] != HT_EMPTY && ht_keys[h] != key)
            h = (h + 1) & HT_MASK;

        if (ht_keys[h] == key) {
            prefix_code = ht_vals[h];
        } else {
            writer.writeBits(prefix_code, bit_width);
            if (nextCode < MAX_DICT_SIZE) {
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
        return { arena.data() + offsets[code], lengths[code] };
    }

    void push(int parent_code, char suffix) {
        auto [ptr, len] = get(parent_code);
        offsets.push_back(static_cast<uint32_t>(arena.size()));
        lengths.push_back(len + 1);
        arena.insert(arena.end(), ptr, ptr + len);
        arena.push_back(suffix);
    }
};

// Main decompression block (same as lzw_parallel.cpp)
size_t decompressLZW(const uint8_t* data, size_t length,
                     char* out_buf, size_t out_buf_size)
{
    if (length == 0) {
        return 0;
    }

    ArenaDict dict;
    BitReader reader(data, length);
    int bit_width = 9, threshold = 512;

    int code = reader.readBits(bit_width);
    if (code == -1) {
        return 0;
    }

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

        if (out_pos + s_len > out_buf_size) {
            break;
        }

        std::memcpy(out_buf + out_pos, s_ptr, s_len);
        out_pos += s_len;
        char suffix = s_ptr[0];
        if (!is_new && dict.size() < MAX_DICT_SIZE) {
            dict.push(old_code, suffix);
        }
        if ((int)dict.size() == threshold - 1 && bit_width < MAX_BITS) { 
            bit_width++; 
            threshold *= 2; 
        }
        old_code = code;
    }
    return out_pos;
}


// New structure to hold file compression result
struct FileResult {
    fs::path filename;
    std::vector<uint8_t> compressed_data;
    size_t original_size;
    bool success;
};

// New structure for decompression
struct DecompressJob {
    fs::path input_file;
    fs::path output_file;
    size_t expected_size;
    std::vector<uint8_t> compressed_data;
};

class FolderCompressor {
private:
    struct FileTask {
        fs::path input_path;
        fs::path output_path;
        size_t file_size;
        std::vector<uint8_t> compressed_data;
        bool success;
    };

    // To ignore .git and other irrelevant files
    bool shouldIgnoreFile(const fs::path& filepath) {
        // Check if filename starts with dot
        std::string filename = filepath.filename().string();
        if (filename.size() > 0 && filename[0] == '.') {
            return true;
        }
        
        // Also check if any parent directory starts with dot
        for (auto it = filepath.begin(); it != filepath.end(); ++it) {
            std::string part = it->string();
            if (part.size() > 0 && part[0] == '.' && part != "." && part != "..") {
                return true;
            }
        }
        
        return false;
    }

    // Main function, (file-level + block-level parallelism)
    std::vector<FileTask> compressFiles(const std::vector<fs::path>& files, const fs::path& output_dir, int num_threads) {
        std::vector<FileTask> tasks(files.size());
        
        // Pre-assign output paths
        for (size_t i = 0; i < files.size(); ++i) {
            tasks[i].input_path = files[i];
            fs::path rel_path = fs::relative(files[i], output_dir.parent_path());
            tasks[i].output_path = output_dir / rel_path;
            tasks[i].output_path += ".lzw";
            tasks[i].success = false;
        }
        
        auto t0 = std::chrono::high_resolution_clock::now();
        
        // 1. Create 1 parallel region for entire process
        #pragma omp parallel num_threads(num_threads)
        {
            // 2. Only 1 thread acts as the "dispatcher" to spawn the initial file tasks
            #pragma omp single
            {
                for (size_t i = 0; i < files.size(); ++i) {
                    
                    // 3. Spawn a task for each file. 'i' is passed by value (firstprivate)
                    #pragma omp task shared(tasks, files) firstprivate(i)
                    {
                        std::error_code ec;
                        tasks[i].file_size = fs::file_size(files[i], ec);
                        
                        if (!ec && tasks[i].file_size == 0) {
                            uint32_t num_blocks = 0;
                            tasks[i].compressed_data.resize(sizeof(num_blocks));
                            memcpy(tasks[i].compressed_data.data(), &num_blocks, sizeof(num_blocks));
                            tasks[i].success = true;
                        } 
                        else if (!ec) {
                            int fd = open(files[i].c_str(), O_RDONLY);
                            if (fd != -1) {
                                uint8_t* mapped_data = static_cast<uint8_t*>(
                                    mmap(nullptr, tasks[i].file_size, PROT_READ, MAP_PRIVATE, fd, 0));
                                
                                if (mapped_data != MAP_FAILED) {
                                    madvise(mapped_data, tasks[i].file_size, MADV_SEQUENTIAL | MADV_WILLNEED);
                                    
                                    size_t total_length = tasks[i].file_size;
                                    uint32_t num_blocks = (total_length + BLOCK_SIZE - 1) / BLOCK_SIZE;
                                    std::vector<std::vector<uint8_t>> compressed_blocks(num_blocks);
                                    
                                    if (total_length < BLOCK_SIZE) {
                                        // Small file: Compress directly in this task
                                        std::vector<uint32_t> ht_keys(HT_SIZE, 0xFFFFFFFFu); // Use HT_EMPTY
                                        std::vector<uint16_t> ht_vals(HT_SIZE);
                                        BitWriter writer(BLOCK_SIZE + BLOCK_SIZE / 2);
                                        
                                        compressed_blocks[0] = compressLZW_Block(
                                            mapped_data, total_length, 
                                            ht_keys.data(), ht_vals.data(), writer);
                                    } else {
                                        // Large file: Spawn sub-tasks for each block
                                        for (size_t j = 0; j < num_blocks; ++j) {
                                            
                                            // 4. Spawn block tasks into the global pool
                                            #pragma omp task shared(compressed_blocks, mapped_data) firstprivate(j, total_length)
                                            {
                                                std::vector<uint32_t> ht_keys(HT_SIZE, 0xFFFFFFFFu);
                                                std::vector<uint16_t> ht_vals(HT_SIZE);
                                                BitWriter local_writer(BLOCK_SIZE + BLOCK_SIZE / 2);
                                                
                                                size_t off = j * BLOCK_SIZE;
                                                size_t len = std::min(BLOCK_SIZE, total_length - off);
                                                compressed_blocks[j] = compressLZW_Block(
                                                    mapped_data + off, len, 
                                                    ht_keys.data(), ht_vals.data(), local_writer);
                                            }
                                        }
                                        // 5. Wait for all blockd spawned by file to finish before assembling
                                        #pragma omp taskwait
                                    }
                                    
                                    // Assemble the compressed file (Header + Blocks)
                                    size_t header_bytes = sizeof(uint32_t) + num_blocks * sizeof(uint32_t);
                                    std::vector<size_t> out_offsets(num_blocks);
                                    out_offsets[0] = header_bytes;
                                    for (size_t j = 1; j < num_blocks; ++j)
                                        out_offsets[j] = out_offsets[j-1] + compressed_blocks[j-1].size();
                                    
                                    size_t total_out = out_offsets[num_blocks-1] + compressed_blocks[num_blocks-1].size();
                                    tasks[i].compressed_data.resize(total_out);
                                    
                                    memcpy(tasks[i].compressed_data.data(), &num_blocks, sizeof(num_blocks));
                                    size_t offset = sizeof(num_blocks);
                                    for (size_t j = 0; j < num_blocks; ++j) {
                                        uint32_t block_size = compressed_blocks[j].size();
                                        memcpy(tasks[i].compressed_data.data() + offset, &block_size, sizeof(block_size));
                                        offset += sizeof(block_size);
                                    }
                                    
                                    for (size_t j = 0; j < num_blocks; ++j) {
                                        memcpy(tasks[i].compressed_data.data() + out_offsets[j], 
                                            compressed_blocks[j].data(), 
                                            compressed_blocks[j].size());
                                    }
                                    
                                    munmap(mapped_data, tasks[i].file_size);
                                }
                                close(fd);
                                tasks[i].success = (mapped_data != MAP_FAILED);
                            }
                        }
                    }
                }
            } // Implicit barrier, ensures all tasks done before timing finishes
        }
        
        auto t1 = std::chrono::high_resolution_clock::now();
        
        // I/O write loop
        #pragma omp parallel for schedule(static) num_threads(num_threads)
        for (size_t i = 0; i < tasks.size(); ++i) {
            if (tasks[i].success) {
                fs::create_directories(tasks[i].output_path.parent_path());
                std::ofstream ofs(tasks[i].output_path, std::ios::binary);
                if (ofs) {
                    ofs.write(reinterpret_cast<const char*>(tasks[i].compressed_data.data()), 
                            tasks[i].compressed_data.size());
                }
            }
        }
        
        std::cerr << "[Parallel compression time: " 
                << std::chrono::duration<double>(t1 - t0).count() 
                << "s]" << std::endl;
        
        return tasks;
    }

    // Build list of decompression jobs from input directory
    std::vector<DecompressJob> prepareDecompressionJobs(
        const fs::path& input_dir, 
        const fs::path& output_dir) {
        
        std::vector<DecompressJob> jobs;
        
        // Look through directory and find .lzw files
        for (const auto& entry : fs::recursive_directory_iterator(input_dir)) {
            if (entry.is_regular_file() && 
                entry.path().extension() == ".lzw" && 
                !shouldIgnoreFile(entry.path())) {
                
                DecompressJob job;
                job.input_file = entry.path();
                
                // Create output with original extension
                fs::path rel_path = fs::relative(entry.path(), input_dir);
                fs::path out_path = output_dir / rel_path;
                out_path.replace_extension("");
                job.output_file = out_path;
                
                // Read compressed file
                std::ifstream ifs(entry.path(), std::ios::binary);
                if (ifs) {
                    ifs.seekg(0, std::ios::end);
                    size_t size = ifs.tellg();
                    ifs.seekg(0, std::ios::beg);

                    job.compressed_data.resize(size);
                    ifs.read(reinterpret_cast<char*>(job.compressed_data.data()), size);
                    job.expected_size = size;
                    jobs.push_back(job);
                }
            }
        }
        
        return jobs;
    }
    
    // Decompress a single file (block-parallel)
    bool decompressFile(const DecompressJob& job) {
        const uint8_t* ptr = job.compressed_data.data();
        
        uint32_t num_blocks;
        memcpy(&num_blocks, ptr, sizeof(num_blocks));
        ptr += sizeof(num_blocks);
        
        std::vector<uint32_t> block_sizes(num_blocks);
        memcpy(block_sizes.data(), ptr, num_blocks * sizeof(uint32_t));
        ptr += num_blocks * sizeof(uint32_t);
        
        std::vector<size_t> block_offsets(num_blocks);
        size_t cur = ptr - job.compressed_data.data();
        for (size_t i = 0; i < num_blocks; ++i) {
            block_offsets[i] = cur;
            cur += block_sizes[i];
        }
        
        const size_t OUT_BUF = BLOCK_SIZE * 2;
        std::vector<std::vector<char>> decomp_bufs(num_blocks, std::vector<char>(OUT_BUF));
        std::vector<size_t> decomp_sizes(num_blocks, 0);
        
        #pragma omp parallel for schedule(dynamic)
        for (size_t i = 0; i < num_blocks; ++i) {
            decomp_sizes[i] = decompressLZW(job.compressed_data.data() + block_offsets[i], block_sizes[i], decomp_bufs[i].data(), OUT_BUF);
        }
        
        // Calculate total size and write output
        std::vector<size_t> out_offsets(num_blocks);
        out_offsets[0] = 0;
        for (size_t i = 1; i < num_blocks; ++i) {
            out_offsets[i] = out_offsets[i-1] + decomp_sizes[i-1];
        }
        size_t total_out = out_offsets[num_blocks-1] + decomp_sizes[num_blocks-1];
        
        // Create output directory if needed
        fs::create_directories(job.output_file.parent_path());
        
        int out_fd = open(job.output_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd == -1) {
            return false;
        }
        
        if (ftruncate(out_fd, static_cast<off_t>(total_out)) == -1) {
            close(out_fd);
            return false;
        }
        
        #pragma omp parallel for schedule(static)
        for (size_t i = 0; i < num_blocks; ++i) {
            if (decomp_sizes[i] == 0) {
                continue;
            }
            pwrite(out_fd, decomp_bufs[i].data(), decomp_sizes[i],static_cast<off_t>(out_offsets[i]));
        }
        
        close(out_fd);
        return true;
    }
    
public:
    // Overall controller of folder comppression
    void compressFolder(const fs::path& input_dir, 
                       const fs::path& output_dir,
                       int num_threads = 4) {
        
        if (!fs::exists(input_dir)) {
            std::cerr << "Input directory does not exist: " << input_dir << std::endl;
            return;
        }
        
        fs::create_directories(output_dir);
        
        // Collect all regular files
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(input_dir)) {
            if (entry.is_regular_file() && !shouldIgnoreFile(entry.path())) {
                files.push_back(entry.path());
            }
        }
        
        std::cerr << "Found " << files.size() << " files to compress" << std::endl;
        
        const size_t BATCH_SIZE = 100; // Process in batches
        int total_success_cnt = 0;
        
        for (size_t batch_start = 0; batch_start < files.size(); batch_start += BATCH_SIZE) {
            size_t batch_end = std::min(batch_start + BATCH_SIZE, files.size());
            std::vector<fs::path> batch(files.begin() + batch_start, files.begin() + batch_end);
            
            auto results = compressFiles(batch, output_dir, num_threads);
            
            for (const auto& result : results) {
                if (result.success) {
                    total_success_cnt++;
                }
            }
        }
        
        std::cerr << "Success! compressed " << total_success_cnt << "/" << files.size() << " files" << std::endl;
    }
    
    // Overall controller of folder decompression
    void decompressFolder(const fs::path& input_dir,
                         const fs::path& output_dir,
                         int num_threads = 4) {
        
        if (!fs::exists(input_dir)) {
            std::cerr << "Input directory does not exist: " << input_dir << std::endl;
            return;
        }
        
        fs::create_directories(output_dir);
        
        auto jobs = prepareDecompressionJobs(input_dir, output_dir);
        
        std::cerr << "Found " << jobs.size() << " .lzw files to decompress" << std::endl;
        
        auto t0 = std::chrono::high_resolution_clock::now();
        
        omp_set_num_threads(num_threads);
        
        #pragma omp parallel for schedule(dynamic)
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (!decompressFile(jobs[i])) {
                #pragma omp critical
                std::cerr << "Decomp failed " << jobs[i].input_file << std::endl;
            }
        }
        
        auto t1 = std::chrono::high_resolution_clock::now();
        std::cerr << "[Parallel decomp time: " << std::chrono::duration<double>(t1 - t0).count() << "s]" << std::endl;
    }
};


// Main loop, parses input arguments and starts appropriate task
int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage:\n";
        std::cerr << "  Compress:   " << argv[0] 
                  << " -c <input_dir> <output_dir> [num_threads]\n";
        std::cerr << "  Decompress: " << argv[0] 
                  << " -d <input_dir> <output_dir> [num_threads]\n";
        return 1;
    }
    
    std::string mode = argv[1];
    fs::path input_dir = argv[2];
    fs::path output_dir = argv[3];
    int num_threads = (argc == 5) ? std::stoi(argv[4]) : omp_get_max_threads();
    
    FolderCompressor compressor;
    
    if (mode == "-c") {
        compressor.compressFolder(input_dir, output_dir, num_threads);
    } else if (mode == "-d") {
        compressor.decompressFolder(input_dir, output_dir, num_threads);
    } else {
        std::cerr << "Invalid mode. Use -c (compress) or -d (decompress).\n";
        return 1;
    }
    
    return 0;
}
