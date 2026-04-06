#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <iomanip>
#include <chrono>

// LZW Compress: Converts a string into a list of dictionary indices
std::vector<int> compressLZW(const std::string& input) {
    std::unordered_map<std::string, int> dict;
    
    // Initialize dictionary with single characters (ASCII 0-255)
    for (int i = 0; i < 256; i++) {
        dict[std::string(1, char(i))] = i;
    }

    std::string p = "";
    std::vector<int> output;
    int nextCode = 256;

    for (char c : input) {
        std::string pc = p + c;
        if (dict.count(pc)) {
            p = pc;
        } else {
            output.push_back(dict[p]);
            // Add new sequence to dictionary
            dict[pc] = nextCode++;
            p = std::string(1, c);
        }
    }

    // Output the last sequence
    if (!p.empty()) {
        output.push_back(dict[p]);
    }

    return output;
}

// LZW Decompress: Converts dictionary indices back into the original string
std::string decompressLZW(const std::vector<int>& input) {
    std::unordered_map<int, std::string> dict;
    for (int i = 0; i < 256; i++) {
        dict[i] = std::string(1, char(i));
    }

    std::string old = dict[input[0]];
    std::string result = old;
    std::string s;
    char c = old[0];
    int nextCode = 256;

    for (size_t i = 1; i < input.size(); i++) {
        int n = input[i];
        if (dict.count(n)) {
            s = dict[n];
        } else {

            s = old + c;
        }
        result += s;
        c = s[0];
        dict[nextCode++] = old + c;
        old = s;
    }

    return result;
}

double calculateRatio(const std::string& text, const std::vector<int>& compressed) {
    if (original.empty()) return 0.0;

    size_t originalSizeBytes = text.length();
    size_t compressedSizeBytes = compressed.size() * sizeof(int);

    return static_cast<double>(compressedSizeBytes) / static_cast<double>(originalSizeBytes);
}

int main() {
    std::string text = "TOBEORNOTTOBEORTOBEORNOT";
    
     auto start = std::chrono::steady_clock::now();

    std::vector<int> result = compressLZW(text);

    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = end - start;

    std::cout << "Computation time (sec): " << std::fixed << std::setprecision(10) << elapsed.count() << "s" << std::endl;


    std::string decompressed = decompressLZW(result);
    double ratio = calculateRatio(text, result);
    
    if (text == decompressed) {
        std::cout << "Original Size:   " << text.length() << " bytes" << std::endl;
        std::cout << "Compressed Size: " << result.size() * sizeof(int) << " bytes" << std::endl;
        std::cout << "Ratio:           " << ratio << std::endl;

    }

    return 0;
}