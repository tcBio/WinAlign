/**
 * @file bam_merger.cpp
 * @brief BAM file merger for parallel WinAlign output (Phase 5)
 *
 * Merges multiple BAM files from parallel winalign processes into a single
 * sorted BAM file. Uses external samtools for efficient merging.
 */

#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <cstdlib>

namespace fs = std::filesystem;

/**
 * @brief Check if samtools is available
 */
bool check_samtools() {
#ifdef _WIN32
    int result = system("samtools --version >nul 2>&1");
#else
    int result = system("samtools --version >/dev/null 2>&1");
#endif
    return result == 0;
}

/**
 * @brief Merge BAM files using samtools
 */
int merge_bam_files(const std::vector<std::string>& input_bams,
                    const std::string& output_bam,
                    uint32_t threads = 4,
                    bool sort_output = true) {

    if (input_bams.empty()) {
        std::cerr << "Error: No input BAM files provided" << std::endl;
        return 1;
    }

    if (input_bams.size() == 1) {
        std::cout << "Only one input file, copying directly..." << std::endl;
        fs::copy_file(input_bams[0], output_bam, fs::copy_options::overwrite_existing);
        return 0;
    }

    std::cout << "Merging " << input_bams.size() << " BAM files..." << std::endl;

    // Create temporary merged file
    std::string temp_merged = output_bam + ".temp.bam";

    // Build samtools merge command
    std::ostringstream merge_cmd;
    merge_cmd << "samtools merge -@ " << threads << " " << temp_merged;

    for (const auto& bam : input_bams) {
        merge_cmd << " " << bam;
    }

    std::string merge_command = merge_cmd.str();
    std::cout << "Running: " << merge_command << std::endl;

    int result = system(merge_command.c_str());
    if (result != 0) {
        std::cerr << "Error: samtools merge failed with code " << result << std::endl;
        return result;
    }

    std::cout << "Merge complete." << std::endl;

    // Sort if requested
    if (sort_output) {
        std::cout << "Sorting merged BAM file..." << std::endl;

        std::ostringstream sort_cmd;
        sort_cmd << "samtools sort -@ " << threads
                 << " -o " << output_bam
                 << " " << temp_merged;

        std::string sort_command = sort_cmd.str();
        std::cout << "Running: " << sort_command << std::endl;

        result = system(sort_command.c_str());
        if (result != 0) {
            std::cerr << "Error: samtools sort failed with code " << result << std::endl;
            fs::remove(temp_merged);
            return result;
        }

        std::cout << "Sort complete." << std::endl;

        // Remove temporary file
        fs::remove(temp_merged);

        // Index the final BAM
        std::cout << "Indexing final BAM file..." << std::endl;

        std::ostringstream index_cmd;
        index_cmd << "samtools index -@ " << threads << " " << output_bam;

        std::string index_command = index_cmd.str();
        std::cout << "Running: " << index_command << std::endl;

        result = system(index_command.c_str());
        if (result != 0) {
            std::cerr << "Warning: samtools index failed with code " << result << std::endl;
            // Don't fail on index error
        } else {
            std::cout << "Index complete." << std::endl;
        }

    } else {
        // Just rename temp to final
        fs::rename(temp_merged, output_bam);
    }

    // Print file size
    auto file_size = fs::file_size(output_bam);
    std::cout << "\nFinal BAM file: " << output_bam << std::endl;
    std::cout << "Size: " << (file_size / (1024.0 * 1024.0)) << " MB" << std::endl;

    return 0;
}

/**
 * @brief Print usage information
 */
void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options] -i <input_pattern> -o <output.bam>\n"
              << "\n"
              << "Options:\n"
              << "  -i, --input <pattern>     Input BAM file pattern (e.g., results/*.bam)\n"
              << "  -o, --output <file>       Output merged BAM file\n"
              << "  -t, --threads <N>         Number of threads (default: 4)\n"
              << "  -n, --no-sort             Skip sorting (merge only)\n"
              << "  -h, --help                Show this help message\n"
              << "\n"
              << "Requirements:\n"
              << "  - samtools must be installed and in PATH\n"
              << "\n"
              << "Examples:\n"
              << "  " << prog_name << " -i results/*.bam -o final.bam -t 8\n"
              << "  " << prog_name << " -i output/chunk*.bam -o merged.bam --no-sort\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    try {
        std::string input_pattern;
        std::string output_bam;
        uint32_t threads = 4;
        bool sort_output = true;

        // Parse arguments
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];

            if ((arg == "-i" || arg == "--input") && i + 1 < argc) {
                input_pattern = argv[++i];
            } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
                output_bam = argv[++i];
            } else if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
                threads = std::stoi(argv[++i]);
            } else if (arg == "-n" || arg == "--no-sort") {
                sort_output = false;
            } else if (arg == "-h" || arg == "--help") {
                print_usage(argv[0]);
                return 0;
            }
        }

        if (input_pattern.empty() || output_bam.empty()) {
            print_usage(argv[0]);
            return 1;
        }

        // Check for samtools
        if (!check_samtools()) {
            std::cerr << "Error: samtools not found in PATH" << std::endl;
            std::cerr << "Please install samtools: http://www.htslib.org/" << std::endl;
            return 1;
        }

        // Find all matching BAM files
        std::vector<std::string> input_bams;
        fs::path pattern_path(input_pattern);
        fs::path parent = pattern_path.parent_path();
        std::string pattern_name = pattern_path.filename().string();

        if (parent.empty()) parent = ".";

        // Handle wildcard pattern
        if (pattern_name.find('*') != std::string::npos) {
            std::string prefix = pattern_name.substr(0, pattern_name.find('*'));
            std::string suffix = pattern_name.substr(pattern_name.find('*') + 1);

            for (const auto& entry : fs::directory_iterator(parent)) {
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();
                    if (filename.find(prefix) == 0 && filename.ends_with(suffix)) {
                        input_bams.push_back(entry.path().string());
                    }
                }
            }
        } else {
            // Exact file or directory
            if (fs::is_directory(input_pattern)) {
                for (const auto& entry : fs::directory_iterator(input_pattern)) {
                    if (entry.path().extension() == ".bam") {
                        input_bams.push_back(entry.path().string());
                    }
                }
            } else {
                input_bams.push_back(input_pattern);
            }
        }

        if (input_bams.empty()) {
            std::cerr << "Error: No BAM files found matching pattern: " << input_pattern << std::endl;
            return 1;
        }

        // Sort input files by name for deterministic ordering
        std::sort(input_bams.begin(), input_bams.end());

        std::cout << "Found " << input_bams.size() << " BAM files to merge:" << std::endl;
        for (const auto& bam : input_bams) {
            auto size = fs::file_size(bam);
            std::cout << "  " << bam << " (" << (size / (1024.0 * 1024.0)) << " MB)" << std::endl;
        }

        // Merge
        return merge_bam_files(input_bams, output_bam, threads, sort_output);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
