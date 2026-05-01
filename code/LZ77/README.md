How to run LZ77 file

Compile LZ77: g++ -O2 -fopenmp -o lz77 lz77.cpp
run: ./lz77 -f {file name} -n #numberThreads

How to run cuda LZ77 file

First: export PATH=/usr/local/cuda-11.7/bin:${PATH}
       export LD_LIBRARY_PATH=/usr/local/cuda-11.7/lib64/:${LD_LIBRARY_PATH}

Comile LZW: nvcc -O2 -std=c++17 -ccbin g++-11 -Xcompiler -fopenmp lz77.cu -o lz77_cuda

run ./lz77_cuda -f {file name} 
