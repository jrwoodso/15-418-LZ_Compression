#include <iostream>
#include <fstream>
#include <thrust/scan.h>
#include <thrust/device_ptr.h>
#include <thrust/execution_policy.h>

// Copies compressed strips into final contiguous output buffer
__global__ void concatenate_kernel(
    unsigned char** d_strip_buffers, int* d_lengths,
    int* d_offsets, unsigned char* d_final_output) 
{
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    int offset = d_offsets[tid];
    int length = d_lengths[tid];
    unsigned char* local_data = d_strip_buffers[tid];

    for(int i = 0; i < length; i++) {
        d_final_output[offset + i] = local_data[i];
    }
}

#define TILE_DIM 32
#define BLOCK_ROWS 8

// Performs transpose using shared memory tiling
__global__ void transpose_kernel(unsigned char *idata, unsigned char *odata, int width, int height) {
    __shared__ unsigned char tile[TILE_DIM][TILE_DIM + 1]; 

    int x = blockIdx.x * TILE_DIM + threadIdx.x;
    int y = blockIdx.y * TILE_DIM + threadIdx.y;

    if (x < width && y < height) {
        for (int j = 0; j < TILE_DIM; j += BLOCK_ROWS) {
            if (y + j < height) {
                tile[threadIdx.y + j][threadIdx.x] = idata[(y + j) * width + x];
            }
        }
    }
    __syncthreads();

    x = blockIdx.y * TILE_DIM + threadIdx.x;  
    y = blockIdx.x * TILE_DIM + threadIdx.y;

    if (x < height && y < width) {
        for (int j = 0; j < TILE_DIM; j += BLOCK_ROWS) {
            if (y + j < width) {
                odata[(y + j) * height + x] = tile[threadIdx.x][threadIdx.y + j];
            }
        }
    }
}

// Compresses one image strip per thread using LZW variant
__global__ void lzw_compress_strip_kernel(
    const unsigned char* __restrict__ d_transposed_image, 
    unsigned char** d_strip_buffers, int* d_lengths,
    unsigned short* d_hash_tables, unsigned short* d_p_tables,    
    unsigned char* d_c_tables, int width, int height) 
{
    int strip_id = blockIdx.x * blockDim.x + threadIdx.x;
    if (strip_id >= height) return;

    unsigned short* hash_table = &d_hash_tables[strip_id * 16384];
    unsigned short* p = &d_p_tables[strip_id * 4096];
    unsigned char* c = &d_c_tables[strip_id * 4096];

    unsigned char time_stamp = 0;
    int current_code = 258;
    unsigned int omega = d_transposed_image[strip_id]; 
    
    int byte_length = 0;
    unsigned char* my_output_buffer = d_strip_buffers[strip_id];

    unsigned int bit_buffer = 0;
    int bit_count = 0;

    auto emit_code = [&](unsigned int code) {
        bit_buffer |= (code << bit_count);
        bit_count += 12; 
        while (bit_count >= 8) {
            my_output_buffer[byte_length++] = (unsigned char)(bit_buffer & 0xFF);
            bit_buffer >>= 8;
            bit_count -= 8;
        }
    };

    // Main loop
    for (int i = 1; i < width; i++) {
        unsigned char x = d_transposed_image[i * height + strip_id];
        unsigned int h = (omega ^ (x << 10) ^ (x >> 4)) & 0x3FFF;
        
        bool found = false;
        unsigned int found_code = 0;
        
        // Hash table logic
        while (true) {
            unsigned short entry = hash_table[h];
            unsigned char entry_ts = entry & 0xF;       
            unsigned int entry_code = entry >> 4;       
            
            if (entry_ts != time_stamp || entry_code == 0) break; 
            
            if (p[entry_code] == omega && c[entry_code] == x) {
                found = true;
                found_code = entry_code;
                break;
            }
            h = (h + 501) & 0x3FFF; 
        }
        
        if (found) {
            omega = found_code;
        } else {
            emit_code(omega);
            if (current_code < 4094) {
                p[current_code] = omega;
                c[current_code] = x;
                hash_table[h] = (current_code << 4) | time_stamp;
                current_code++;
            }
            omega = x;
            
            if (current_code == 4094) {
                emit_code(256); 
                current_code = 258;
                time_stamp = (time_stamp + 1) & 0xF; 
                if (time_stamp == 0) {
                    for(int k = 0; k < 16384; k++) hash_table[k] = 0;
                }
            }
        }
    }
    
    emit_code(omega);
    emit_code(257); 
    
    // Strict flush boundary to prevent Decompressor desync
    if (bit_count > 0) {
        my_output_buffer[byte_length++] = (unsigned char)(bit_buffer & 0xFF);
    }
    
    d_lengths[strip_id] = byte_length;
}

// Computes exclusive prefix sum of strip lengths
void compute_offsets(int* d_lengths, int* d_offsets, int num_strips) {
    thrust::device_ptr<int> dev_ptr_lengths(d_lengths);
    thrust::device_ptr<int> dev_ptr_offsets(d_offsets);
    thrust::exclusive_scan(thrust::device, dev_ptr_lengths, dev_ptr_lengths + num_strips, dev_ptr_offsets);
}

// Main logic, takes cmd line args, loads and mallocs, transposes, compresses, writes out to disk
int main(int argc, char* argv[]) {
    if (argc < 3) return 1;

    const char* input_filename = argv[1];
    const char* output_filename = argv[2];

    int width = 4096, height = 3072; 
    int num_strips = height;
    int image_bytes = width * height * sizeof(unsigned char);

    unsigned char *d_original_image, *d_transposed_image;
    cudaMalloc(&d_original_image, image_bytes);
    cudaMalloc(&d_transposed_image, image_bytes);
    
    unsigned char *h_image = (unsigned char*)malloc(image_bytes);
    FILE *f = fopen(input_filename, "rb");
    size_t bytes_read = fread(h_image, 1, image_bytes, f);
    if (bytes_read != image_bytes) {
        std::cerr << "Error: expected " << image_bytes << " bytes, but only read " << bytes_read << "\n";
        return 1;
    }
    fclose(f);

    cudaMemcpy(d_original_image, h_image, image_bytes, cudaMemcpyHostToDevice);

    dim3 dimBlockT(32, 8);
    dim3 dimGridT((width + 32 - 1) / 32, (height + 32 - 1) / 32);
    transpose_kernel<<<dimGridT, dimBlockT>>>(d_original_image, d_transposed_image, width, height);

    // Increased max size guarantee for highly incompressible noisy datasets like input2.raw
    int max_strip_compressed_size = (width * 2) + 256; 
    
    unsigned char** h_strip_buffers = new unsigned char*[num_strips];
    for(int i = 0; i < num_strips; i++) cudaMalloc((void**)&h_strip_buffers[i], max_strip_compressed_size);

    unsigned char** d_strip_buffers;
    cudaMalloc((void***)&d_strip_buffers, num_strips * sizeof(unsigned char*));
    cudaMemcpy(d_strip_buffers, h_strip_buffers, num_strips * sizeof(unsigned char*), cudaMemcpyHostToDevice);

    int *d_lengths, *d_offsets;
    cudaMalloc(&d_lengths, num_strips * sizeof(int));
    cudaMalloc(&d_offsets, num_strips * sizeof(int));

    cudaEvent_t start, stop;
    cudaEventCreate(&start); cudaEventCreate(&stop);
    cudaEventRecord(start);

    int threadsPerBlock = 128;
    int blocksPerGrid = (num_strips + threadsPerBlock - 1) / threadsPerBlock;
   
    unsigned short *d_hash_tables, *d_p_tables;
    unsigned char *d_c_tables;
    cudaMalloc(&d_hash_tables, num_strips * 16384 * sizeof(unsigned short));
    cudaMalloc(&d_p_tables, num_strips * 4096 * sizeof(unsigned short));
    cudaMalloc(&d_c_tables, num_strips * 4096 * sizeof(unsigned char));
    cudaMemset(d_hash_tables, 0, num_strips * 16384 * sizeof(unsigned short));

    // Main compression kernel launch with speicified parameters and dimensions
    lzw_compress_strip_kernel<<<blocksPerGrid, threadsPerBlock>>>(
        d_transposed_image, d_strip_buffers, d_lengths, 
        d_hash_tables, d_p_tables, d_c_tables, width, height
    );

    cudaEventRecord(stop); cudaEventSynchronize(stop);
    float milliseconds = 0;
    cudaEventElapsedTime(&milliseconds, start, stop);
    std::cout << "Kernel execution time: " << milliseconds << " ms\n";

    compute_offsets(d_lengths, d_offsets, num_strips);

    int last_length, last_offset;
    cudaMemcpy(&last_length, &d_lengths[num_strips - 1], sizeof(int), cudaMemcpyDeviceToHost);
    cudaMemcpy(&last_offset, &d_offsets[num_strips - 1], sizeof(int), cudaMemcpyDeviceToHost);

    int total_compressed_size = last_offset + last_length;
    unsigned char* d_final_output;
    cudaMalloc(&d_final_output, total_compressed_size);

    concatenate_kernel<<<blocksPerGrid, threadsPerBlock>>>(d_strip_buffers, d_lengths, d_offsets, d_final_output);
    cudaDeviceSynchronize();

    // Write output to file and cleanup
    unsigned char* host_output = new unsigned char[total_compressed_size];
    cudaMemcpy(host_output, d_final_output, total_compressed_size, cudaMemcpyDeviceToHost);
    std::ofstream out_file(output_filename, std::ios::binary);
    out_file.write(reinterpret_cast<char*>(host_output), total_compressed_size);
    out_file.close();

    delete[] host_output;
    for(int i = 0; i < num_strips; i++) cudaFree(h_strip_buffers[i]);
    delete[] h_strip_buffers;
    cudaFree(d_strip_buffers); cudaFree(d_original_image); cudaFree(d_transposed_image);
    cudaFree(d_lengths); cudaFree(d_offsets); cudaFree(d_final_output);
    cudaFree(d_hash_tables); cudaFree(d_p_tables); cudaFree(d_c_tables);
    free(h_image);
    return 0;
}
