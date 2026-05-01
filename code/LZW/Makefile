CXX      := g++
NVCC     := nvcc

CXXFLAGS := -std=c++17 -O3 -Wall -Wextra
OMPFLAGS := -fopenmp
NVFLAGS  := -O3 -arch=sm_75 -std=c++17 -ccbin=g++-11

TARGETS  := lzw_parallel lzw_file_parallel \
            lzw_gpu_compress lzw_gpu_decompress

.PHONY: all clean

all: $(TARGETS)

# lzw: lzw.cpp
# 	$(CXX) $(CXXFLAGS) -o $@ $<

lzw_parallel: lzw_parallel.cpp
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) -o $@ $<

# lzw_parallel_psc: lzw_parallel_psc.cpp
# 	$(CXX) $(CXXFLAGS) $(OMPFLAGS) -o $@ $<

lzw_file_parallel: lzw_file_parallel.cpp
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) -o $@ $<

# GPU targets
lzw_gpu_compress: lzw_gpu_compress.cu
	$(NVCC) $< -o $@ $(NVFLAGS)

lzw_gpu_decompress: lzw_gpu_decompress.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)
