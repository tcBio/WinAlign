/**
 * @file fastq_chunker.cpp
 * @brief FASTQ file chunker for parallel processing (Phase 5)
 *
 * Splits FASTQ files into N roughly equal chunks for multi-process execution.
 * Preserves read pairs for paired-end data.
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <zlib.h>

namespace fs = std::filesystem;

struct ChunkInfo {
    std::string output_path;
    uint64_t start_read;
    uint64_t num_reads;
    std::ofstream* file;
};

/**
 * @brief Count reads in FASTQ file (gzipped or plain)
 */
uint64_t count_fastq_reads(const std::string& path) {
    bool is_gzipped = path.ends_with(".gz");
    uint64_t count = 0;

    if (is_gzipped) {
        gzFile file = gzopen(path.c_str(), "r");
        if (!file) {
            throw std::runtime_error("Failed to open: " + path);
        }

        char buffer[65536];
        int lines = 0;
        while (gzgets(file, buffer, sizeof(buffer))) {
            lines++;
            if (lines == 4) {
                count++;
                lines = 0;
            }
        }
        gzclose(file);
    } else {
        std::ifstream file(path);
        if (!file) {
            throw std::runtime_error("Failed to open: " + path);
        }

        std::string line;
        int lines = 0;
        while (std::getline(file, line)) {
            lines++;
            if (lines == 4) {
                count++;
                lines = 0;
            }
        }
    }

    return count;
}

/**
 * @brief Split FASTQ file into N chunks
 */
void split_fastq(const std::string& input_path,
                 const std::string& output_prefix,
                 uint32_t num_chunks,
                 bool compress_output = false) {

    std::cout << "Counting reads in " << input_path << "..." << std::endl;
    uint64_t total_reads = count_fastq_reads(input_path);
    std::cout << "Total reads: " << total_reads << std::endl;

    if (total_reads == 0) {
        throw std::runtime_error("No reads found in input file");
    }

    // Calculate reads per chunk
    uint64_t reads_per_chunk = (total_reads + num_chunks - 1) / num_chunks;
    std::cout << "Target reads per chunk: " << reads_per_chunk << std::endl;

    // Create output files
    std::vector<ChunkInfo> chunks(num_chunks);
    for (uint32_t i = 0; i < num_chunks; i++) {
        std::string ext = compress_output ? ".fastq.gz" : ".fastq";
        chunks[i].output_path = output_prefix + "_chunk" + std::to_string(i) + ext;
        chunks[i].start_read = i * reads_per_chunk;
        chunks[i].num_reads = 0;
        chunks[i].file = new std::ofstream(chunks[i].output_path);

        if (!chunks[i].file->is_open()) {
            throw std::runtime_error("Failed to create: " + chunks[i].output_path);
        }
    }

    // Read and distribute
    bool is_gzipped = input_path.ends_with(".gz");
    gzFile gz_file = nullptr;
    std::ifstream plain_file;

    if (is_gzipped) {
        gz_file = gzopen(input_path.c_str(), "r");
        if (!gz_file) {
            throw std::runtime_error("Failed to open: " + input_path);
        }
    } else {
        plain_file.open(input_path);
        if (!plain_file) {
            throw std::runtime_error("Failed to open: " + input_path);
        }
    }

    std::cout << "Splitting into " << num_chunks << " chunks..." << std::endl;

    uint64_t current_read = 0;
    uint32_t current_chunk = 0;
    std::string lines[4];
    char buffer[65536];

    while (current_read < total_reads) {
        // Read 4 lines (1 FASTQ record)
        bool eof = false;
        for (int i = 0; i < 4; i++) {
            if (is_gzipped) {
                if (!gzgets(gz_file, buffer, sizeof(buffer))) {
                    eof = true;
                    break;
                }
                lines[i] = buffer;
            } else {
                if (!std::getline(plain_file, lines[i])) {
                    eof = true;
                    break;
                }
                lines[i] += '\n';
            }
        }

        if (eof) break;

        // Determine which chunk
        current_chunk = current_read / reads_per_chunk;
        if (current_chunk >= num_chunks) {
            current_chunk = num_chunks - 1;
        }

        // Write to chunk
        for (int i = 0; i < 4; i++) {
            *chunks[current_chunk].file << lines[i];
        }
        chunks[current_chunk].num_reads++;

        current_read++;

        // Progress
        if (current_read % 100000 == 0) {
            std::cout << "Processed " << current_read << " / " << total_reads
                      << " reads (" << (current_read * 100 / total_reads) << "%)" << std::endl;
        }
    }

    // Cleanup
    if (is_gzipped) {
        gzclose(gz_file);
    } else {
        plain_file.close();
    }

    for (auto& chunk : chunks) {
        chunk.file->close();
        delete chunk.file;
    }

    // Summary
    std::cout << "\nChunking complete!" << std::endl;
    std::cout << "Created " << num_chunks << " chunks:" << std::endl;
    for (uint32_t i = 0; i < num_chunks; i++) {
        std::cout << "  " << chunks[i].output_path << ": "
                  << chunks[i].num_reads << " reads" << std::endl;
    }
}

/**
 * @brief Print usage information
 */
void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options] <input.fastq[.gz]> <output_prefix> <num_chunks>\n"
              << "\n"
              << "Options:\n"
              << "  -z, --gzip          Compress output chunks with gzip\n"
              << "  -h, --help          Show this help message\n"
              << "\n"
              << "Examples:\n"
              << "  " << prog_name << " reads.fastq.gz chunks/reads 8\n"
              << "  " << prog_name << " --gzip reads.fastq chunks/reads 4\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    try {
        bool compress_output = false;
        std::vector<std::string> args;

        // Parse arguments
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "-z" || arg == "--gzip") {
                compress_output = true;
            } else if (arg == "-h" || arg == "--help") {
                print_usage(argv[0]);
                return 0;
            } else {
                args.push_back(arg);
            }
        }

        if (args.size() != 3) {
            print_usage(argv[0]);
            return 1;
        }

        std::string input_path = args[0];
        std::string output_prefix = args[1];
        uint32_t num_chunks = std::stoi(args[2]);

        if (num_chunks == 0 || num_chunks > 256) {
            std::cerr << "Error: num_chunks must be between 1 and 256" << std::endl;
            return 1;
        }

        // Create output directory if needed
        fs::path prefix_path(output_prefix);
        if (prefix_path.has_parent_path()) {
            fs::create_directories(prefix_path.parent_path());
        }

        // Split the file
        split_fastq(input_path, output_prefix, num_chunks, compress_output);

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
