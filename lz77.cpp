#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <iomanip>
#include <chrono>


struct Node{
    int offset;
    int len;
    char next;
};




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
                    // stop if the current position 'i' is hit
                    
                    if (j + k == i) break; 
                }

                if (k >= matchLength) {
                    matchLength = k;
                    matchOffset = i - j;
                }
            }

            // Move the cursor forward by the match length + 1
            char nextC = (i + matchLength < len) ? text[i + matchLength] : '\0';
            compressedText.push_back({matchOffset, matchLength, nextC});
            
            i += matchLength + 1;
        }
    return compressedText;
    }






std::string decompress(const std::vector<Node>& compressedText) {
    std::string result = "";

    for (const auto& node : compressedText) {
        if (node.len > 0) {
            // Find the starting point for the copy
            int start = result.length() - node.offset;
            
            for (int i = 0; i < node.len; ++i) {
                // We add characters one by one. This handles cases 
                // where the length is greater than the offset (overlapping).
                result += result[start + i];
            }
        }
        
        // Add the literal character if it's not the null terminator
        if (node.next != '\0') {
            result += node.next;
        }
    }

    return result;
}

double compressionRatio(const std::string& original, const std::vector<Node>& compressed) {
    if (original.empty()) return 0.0;

    size_t originalSize = original.length(); // 1 byte per char
    size_t compressedSize = compressed.size() * sizeof(Node);

    return static_cast<double>(compressedSize) / static_cast<double>(originalSize);
}


int main() {
    std::string text = "abacabadabacabaeabacabadabacabaeabacabadabacabaeabacabadabe";
    int slidingWindow = 10;

    auto start = std::chrono::steady_clock::now();

   


    std::vector<Node> result = compress(text,slidingWindow);

    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = end - start;

    std::cout << "Computation time (sec): " << std::fixed << std::setprecision(10) << elapsed.count() << "s" << std::endl;
  
    std::string Original = decompress(result);
    double ratio = compressionRatio(text, result);
    std::cout << Original << "\n" << std::endl;
    if (Original == text){
        std::cout << "Compression successful" << std::endl;
        std::cout << "Original Size: " << text.length() << " bytes" << std::endl;
        std::cout << "Compressed Size: " << result.size() * sizeof(Node) << " bytes" << std::endl;
        std::cout << "Compression Ratio: " << ratio << std::endl;
        std::cout << "Space Savings: " << (1.0 - ratio) * 100 << "%" << std::endl;
        
    }
    else {
        std::cout << "Not Properly Compressed" << std::endl;
    }

    return 0;
}