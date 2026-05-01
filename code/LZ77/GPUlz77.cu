#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <queue>
#include <unistd.h>
#include <cuda_runtime.h>

// Window Size for LZ77
// Block size is for CUDA threads
// Look ahead is buffer size of LZ77 algorithm
#define MAX_LOOKAHEAD 255
#define WINDOW_SIZE 4096
#define BLOCK_SIZE 128
#define MIN_MATCH 3



// Data strucutre to hold huffman
struct HuffNode {
    uint8_t ch;
    int freq;
    HuffNode *left, *right;
    
    HuffNode(uint8_t c, int f) : ch(c), freq(f), left(nullptr), right(nullptr) {}
    HuffNode(HuffNode* l, HuffNode* r) : ch(0), freq(l->freq + r->freq), left(l), right(r) {}
    ~HuffNode() { delete left; delete right; }
};

struct HuffCompare {
    bool operator()(HuffNode* l, HuffNode* r) { return l->freq > r->freq; }
};

void generateCodes(HuffNode* node, uint32_t currentCode, int currentLen, 
                   std::vector<uint32_t>& codes, std::vector<int>& codeLens) {
    if (!node) return;
    if (!node->left && !node->right) {
        codes[node->ch] = currentCode;
        codeLens[node->ch] = currentLen;
        if (currentLen == 0) codeLens[node->ch] = 1; 
        return;
    }
    generateCodes(node->left, currentCode, currentLen + 1, codes, codeLens);
    generateCodes(node->right, currentCode | (1 << currentLen), currentLen + 1, codes, codeLens);
}

std::vector<uint8_t> huffmanEncode(const std::vector<uint8_t>& input) {
    if (input.empty()) return {};

    std::vector<int> freqs(256, 0);
    for (uint8_t byte : input) freqs[byte]++;

    std::priority_queue<HuffNode*, std::vector<HuffNode*>, HuffCompare> pq;
    for (int i = 0; i < 256; i++) {
        if (freqs[i] > 0) pq.push(new HuffNode((uint8_t)i, freqs[i]));
    }

    while (pq.size() > 1) {
        HuffNode* left = pq.top(); pq.pop();
        HuffNode* right = pq.top(); pq.pop();
        pq.push(new HuffNode(left, right));
    }
    HuffNode* root = pq.top();

    std::vector<uint32_t> codes(256, 0);
    std::vector<int> codeLens(256, 0);
    generateCodes(root, 0, 0, codes, codeLens);

    std::vector<uint8_t> out;
    uint8_t uniqueChars = 0;
    for (int f : freqs) if (f > 0) uniqueChars++;
    out.push_back(uniqueChars);
    
    for (int i = 0; i < 256; i++) {
        if (freqs[i] > 0) {
            out.push_back((uint8_t)i);
            out.push_back((freqs[i] >> 0) & 0xFF);
            out.push_back((freqs[i] >> 8) & 0xFF);
            out.push_back((freqs[i] >> 16) & 0xFF);
            out.push_back((freqs[i] >> 24) & 0xFF);
        }
    }

    uint32_t originalSize = input.size();
    out.push_back((originalSize >> 0) & 0xFF);
    out.push_back((originalSize >> 8) & 0xFF);
    out.push_back((originalSize >> 16) & 0xFF);
    out.push_back((originalSize >> 24) & 0xFF);

    uint8_t bitBuffer = 0;
    int bitCount = 0;
    
    for (uint8_t byte : input) {
        uint32_t code = codes[byte];
        int len = codeLens[byte];
        
        for (int i = 0; i < len; i++) {
            if ((code >> i) & 1) bitBuffer |= (1 << bitCount);
            bitCount++;
            if (bitCount == 8) {
                out.push_back(bitBuffer);
                bitBuffer = 0;
                bitCount = 0;
            }
        }
    }
    if (bitCount > 0) out.push_back(bitBuffer); 

    delete root;
    return out;
}

std::vector<uint8_t> huffmanDecode(const std::vector<uint8_t>& in) {
    if (in.empty()) return {};

    int idx = 0;
    int uniqueChars = in[idx++];
    if (uniqueChars == 0) uniqueChars = 256; 

    std::priority_queue<HuffNode*, std::vector<HuffNode*>, HuffCompare> pq;
    for (int i = 0; i < uniqueChars; i++) {
        uint8_t ch = in[idx++];
        int freq = in[idx] | (in[idx+1] << 8) | (in[idx+2] << 16) | (in[idx+3] << 24);
        idx += 4;
        pq.push(new HuffNode(ch, freq));
    }

    if (pq.empty()) return {};
    while (pq.size() > 1) {
        HuffNode* left = pq.top(); pq.pop();
        HuffNode* right = pq.top(); pq.pop();
        pq.push(new HuffNode(left, right));
    }
    HuffNode* root = pq.top();

    uint32_t originalSize = in[idx] | (in[idx+1] << 8) | (in[idx+2] << 16) | (in[idx+3] << 24);
    idx += 4;

    std::vector<uint8_t> result;
    result.reserve(originalSize);
    
    HuffNode* curr = root;
    while (idx < in.size() && result.size() < originalSize) {
        uint8_t bitBuffer = in[idx++];
        for (int bitCount = 0; bitCount < 8 && result.size() < originalSize; bitCount++) {
            if ((bitBuffer >> bitCount) & 1) curr = curr->right;
            else curr = curr->left;

            if (!curr->left && !curr->right) {
                result.push_back(curr->ch);
                curr = root; 
            }
        }
    }

    delete root;
    return result;
}

// Bit Packing algorithm
// If the cost of creating a node to hold data just encode as a byte
std::vector<uint8_t> encodeByteStream(
    const std::string& text,
    const uint16_t* offsets,
    const uint8_t* lengths
) {
    std::vector<uint8_t> out;
    out.reserve(text.size());
    int len = (int)text.size();
    int i = 0;

    while (i < len) {
        int flagPos = out.size();
        out.push_back(0); 
        uint8_t flags = 0;

        for (int bitCount = 0; bitCount < 8 && i < len; bitCount++) {
            uint8_t mLen = lengths[i];
            uint16_t mOff = offsets[i];

            if (mLen >= MIN_MATCH) {
                flags |= (1 << bitCount); 
                out.push_back(mOff & 0xFF);
                out.push_back((mOff >> 8) & 0xFF);
                out.push_back(mLen);
                i += mLen; 
            } else {
                out.push_back((uint8_t)text[i]);
                i++;
            }
        }
        out[flagPos] = flags; 
    }
    return out;
}

// Find longest match in window
__global__ void lz77FindMatches(
    const char* __restrict__ text,
    int len, int windowSize, int lookaheadSize,
    uint16_t* offsets, uint8_t* lengths
) {
    int i = blockIdx.x;
    if (i >= len) return;

    int tid = threadIdx.x;
    int wStart = max(0, i - windowSize);
    int wLen = i - wStart;

    // Shared memory for block-wide reduction
    __shared__ int sLen[BLOCK_SIZE];
    __shared__ int sOff[BLOCK_SIZE];
    sLen[tid] = 0;
    sOff[tid] = 0;
    __syncthreads();

    // Check window
    for (int idx = tid; idx < wLen; idx += blockDim.x) {
        int j = wStart + idx;
        int k = 0;
        
        // Find match length at position j
        while (i + k < len && k < lookaheadSize && text[j + k] == text[i + k])
            k++;
            
        // Store best local match for this thread
        if (k > sLen[tid]) { 
            sLen[tid] = k; 
            sOff[tid] = i - j; 
        }
    }
    __syncthreads();

    // Parallel Reduction to find the absolute best match in the window
    for (int stride = blockDim.x / 2; stride > 0; stride >>= 1) {
        if (tid < stride && sLen[tid + stride] > sLen[tid]) {
            sLen[tid] = sLen[tid + stride];
            sOff[tid] = sOff[tid + stride];
        }
        __syncthreads();
    }

    // Thread 0 writes the global best match for position i to memory
    if (tid == 0) {
        offsets[i] = (uint16_t)sOff[0];
        lengths[i] = (uint8_t) sLen[0];
    }
}

// Compression Algorithm
std::vector<uint8_t> compressCUDA(
    const std::string& text,
    int windowSize = WINDOW_SIZE, int lookaheadSize = MAX_LOOKAHEAD
) {
    int len = (int)text.size();
    if (len == 0) return {};

    char* d_text = nullptr;
    uint16_t* d_offsets = nullptr;
    uint8_t* d_lengths = nullptr;

  
    cudaMalloc(&d_text, len);
    cudaMalloc(&d_offsets, len * sizeof(uint16_t));
    cudaMalloc(&d_lengths, len * sizeof(uint8_t));
    

    cudaMemcpy(d_text, text.data(), len, cudaMemcpyHostToDevice);

    lz77FindMatches<<<len, BLOCK_SIZE>>>(
        d_text, len, windowSize, lookaheadSize, d_offsets, d_lengths
    );
   
    cudaDeviceSynchronize();

    
    std::vector<uint16_t> offsets(len);
    std::vector<uint8_t> lengths(len);
    cudaMemcpy(offsets.data(), d_offsets, len * sizeof(uint16_t), cudaMemcpyDeviceToHost);
    cudaMemcpy(lengths.data(), d_lengths, len * sizeof(uint8_t), cudaMemcpyDeviceToHost);

    cudaFree(d_text);
    cudaFree(d_offsets);
    cudaFree(d_lengths);

    std::vector<uint8_t> lzssOut = encodeByteStream(text, offsets.data(), lengths.data());
    return huffmanEncode(lzssOut);
}


std::string decompress(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> lzssData = huffmanDecode(in);
    
    std::string result;
    int i = 0;
    while (i < (int)lzssData.size()) {
        uint8_t flags = lzssData[i++]; 
        
        for (int bit = 0; bit < 8 && i < (int)lzssData.size(); bit++) {
            if ((flags & (1 << bit)) == 0) {
                result += (char)lzssData[i++];
            } else {
                if (i + 2 >= (int)lzssData.size()) break; 
                
                uint16_t offset = lzssData[i] | ((uint16_t)lzssData[i+1] << 8);
                uint8_t  mLen   = lzssData[i+2];
                i += 3;
                
                int start = (int)result.size() - offset;
                if (start < 0) start = 0; 
                for (int k = 0; k < mLen; ++k) {
                    result += result[start + k];
                }
            }
        }
    }
    return result;
}

double compressionRatio(const std::string& original, const std::vector<uint8_t>& compressed) {
    if (original.size() == 0) return 0.0;
    return static_cast<double>(compressed.size()) / static_cast<double>(original.size());
}


namespace fs = std::filesystem;
static int totalSize = 0;
static int cSize = 0;

// Takes in input file to compress
void runBenchmark(const std::string& filename, std::string& text) {
    std::cout << "\n--- Testing File: " << filename << " ---\n";
  
    auto t0 = std::chrono::steady_clock::now();
    
    std::vector<uint8_t> result = compressCUDA(text);

    auto t1 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    
    std::string decompressed = decompress(result);
    double ratio = compressionRatio(text, result);
    cSize += (int)result.size();
    totalSize += (int)text.size();
    
    std::cout << "Computation time: " << std::fixed << std::setprecision(6) << elapsed << "s\n";
    std::cout << "Status: " << (decompressed == text ? "SUCCESS" : "FAILED") << "\n";
    std::cout << "Original Size: " << text.size() << " bytes\n";
    std::cout << "Compressed Size: " << result.size() << " bytes\n";
    std::cout << "Ratio: " << ratio << "\n";
    
}


int main(int argc, char* argv[]) {
    std::string path;

    int opt;
    while ((opt = getopt(argc, argv, "f:")) != -1) {
        if (opt == 'f') path = optarg;
        else {
            std::cerr << "Usage: " << argv[0] << " -f <file_or_dir>\n";
            exit(EXIT_FAILURE);
        }
    }

    if (path.empty() || !fs::exists(path)) {
        std::cerr << "Valid path required. Usage: " << argv[0] << " -f <file_or_dir>\n";
        return 1;
    }

    if (fs::is_directory(path)) {
        std::cout << "Processing Directory: " << path << "\n";
        
        auto t0 = std::chrono::steady_clock::now();

        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.is_regular_file()) {
                std::ifstream fin(entry.path(), std::ios::binary);
                std::string content((std::istreambuf_iterator<char>(fin)),
                                     std::istreambuf_iterator<char>());
                if (!content.empty())
                    runBenchmark(entry.path().filename().string(), content);
            }
        }

        auto t1 = std::chrono::steady_clock::now();
        std::cout << "\nTotal time: " << std::chrono::duration<double>(t1 - t0).count() << "s\n";
        std::cout << "Total Compression Ratio: "
                  << static_cast<double>(cSize) / static_cast<double>(totalSize) << "\n";
    } else {
        std::ifstream fin(path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(fin)),
                             std::istreambuf_iterator<char>());
        runBenchmark(path, content);
    }

    return 0;
}