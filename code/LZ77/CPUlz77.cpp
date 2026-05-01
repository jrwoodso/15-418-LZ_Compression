#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <unistd.h>

#pragma pack(push, 1)
struct Node {
    uint16_t offset;  
    uint8_t len;     
    char next;        
};
#pragma pack(pop)


std::vector<Node> compress(const std::string& text, const int windowSize) {
    int len = text.length();
    std::vector<Node> compressedText;

    for (int i = 0; i < len;) {
            int matchLength = 0;
            int matchOffset = 0;

            //  start of search buffer (sliding window)
            int start = std::max(0, i - windowSize);

            // Look for the longest match between the search buffer and the look-ahead buffer
            for (int j = start; j < i; j++) {
                int k = 0;
                // Count how many characters match starting from position j
                while (i + k < len && text[j + k] == text[i + k] && k < windowSize) {
                    k++;
                   
                }

                if (k >= matchLength) {
                    matchLength = k;
                    matchOffset = i - j;
                }
            }

            // Move the cursor forward by the match length + 1
            char nextC = (i + matchLength < len) ? text[i + matchLength] : '\0';
            compressedText.push_back({(uint16_t)matchOffset, (uint8_t)matchLength, nextC});
            
            i += matchLength + 1;
        }
    return compressedText;
    }

std::vector<Node> compressParallel(const std::string& text, int windowSize, int numThreads = 0) {
    int len = text.length();
    if (len == 0) return {};

    if (numThreads <= 0)
        numThreads = 1;

    int chunkSize = (len + numThreads - 1) / numThreads;

    std::vector<std::vector<Node>> results(numThreads);

    #pragma omp parallel for num_threads(numThreads) schedule(dynamic)
    for (int t = 0; t < numThreads; t++) {
        int start = t * chunkSize;
        int end   = std::min(start + chunkSize, len);
        if (start >= len) continue;

      
        std::string chunk = text.substr(start, end - start);

        results[t] = compress(chunk, windowSize);

    
    }

    std::vector<Node> compressed;
    compressed.reserve(len);
    for (auto& r : results)
        compressed.insert(compressed.end(), r.begin(), r.end());

    return compressed;
}


std::string decompress(const std::vector<Node>& compressedText) {
    std::string result;
    for (const auto& node : compressedText) {
        if (node.len > 0) {
            int start = (int)result.length() - node.offset;
            for (int i = 0; i < node.len; ++i)
                result += result[start + i];
        }
        if (node.next != '\0')
            result += node.next;
    }
    return result;
}



double compressionRatio(const std::string& original, const std::vector<Node>& compressed) {
    if (original.empty()) return 0.0;
    return static_cast<double>(compressed.size() * sizeof(Node)) /
           static_cast<double>(original.length());
}

namespace fs = std::filesystem;
int totalSize = 0;
int cSize = 0;
// Helper function to run the actual test logic
void runBenchmark(const std::string& filename, const std::string& text, int num_threads) {
    std::cout << "\n--- Testing File: " << filename << " ---" << std::endl;
    
    auto start = std::chrono::steady_clock::now();

    std::vector<Node> result = compressParallel(text, 4096, num_threads);
    auto end = std::chrono::steady_clock::now();

    double elapsed = std::chrono::duration<double>(end - start).count();

    std::string decompressed = decompress(result);
    double ratio = compressionRatio(text, result);
    cSize += result.size() *sizeof(Node);
    totalSize += text.length();
    
    std::cout << "Computation time: " << std::fixed << std::setprecision(6) << elapsed << "s\n";
    std::cout << "Status: " << (decompressed == text ? "SUCCESS" : "FAILED") << "\n";
    std::cout << "Original Size: " << text.size() << " bytes\n";
    std::cout << "Compressed Size: " << result.size() << " bytes\n";
    std::cout << "Ratio: " << ratio << "\n";
    
}

int main(int argc, char* argv[]) {
    std::string path;
    int num_threads = 1;

    int opt;
    while ((opt = getopt(argc, argv, "f:n:")) != -1) {
        switch (opt) {
            case 'f': path = optarg; break;
            case 'n': num_threads = atoi(optarg); break;
            default:
                std::cerr << "Usage: " << argv[0] << " -f <file_or_dir> -n <threads>\n";
                exit(EXIT_FAILURE);
        }
    }

    if (!fs::exists(path)) {
        std::cerr << "Path does not exist: " << path << std::endl;
        return 1;
    }

    // Check if the path is a directory
    if (fs::is_directory(path)) {
        std::cout << "Processing Directory: " << path << std::endl;
        auto start = std::chrono::steady_clock::now();
   
        for (const auto& entry : fs::directory_iterator(path)) {
            // Only process regular files (ignore subfolders, etc.)
            if (entry.is_regular_file()) {
                std::ifstream fin(entry.path());
                std::string content((std::istreambuf_iterator<char>(fin)), 
                                    std::istreambuf_iterator<char>());
                
                if (!content.empty()) {
                    runBenchmark(entry.path().filename().string(), content, num_threads);
                }
            }
        }
        auto end = std::chrono::steady_clock::now();

        double elapsed = std::chrono::duration<double>(end - start).count();
        std::cout << "Total Compression Ratio: " << static_cast<double>(cSize) /
           static_cast<double>(totalSize);
    } else {
       
        std::ifstream fin(path);
        std::string content((std::istreambuf_iterator<char>(fin)), 
                            std::istreambuf_iterator<char>());
        runBenchmark(path, content, num_threads);
    }

    return 0;
}