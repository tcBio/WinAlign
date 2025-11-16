#include "winalign/pipeline.h"
#include "winalign/common.h"
#include "winalign/logger.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <string>
#include <cstdlib>
#include <filesystem>

using namespace winalign;

namespace {
std::string trim_copy(const std::string& input) {
    auto begin = std::find_if_not(input.begin(), input.end(),
                                  [](unsigned char ch) { return std::isspace(ch); });
    auto end = std::find_if_not(input.rbegin(), input.rend(),
                                [](unsigned char ch) { return std::isspace(ch); }).base();
    if (begin >= end) return {};
    return std::string(begin, end);
}

bool load_config_file(const std::string& path, PipelineConfig& config, std::string& error) {
    std::ifstream in(path);
    if (!in.is_open()) {
        error = "Failed to open config file: " + path;
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = trim_copy(line);
        if (line.empty() || line[0] == '#') continue;

        auto delim = line.find('=');
        if (delim == std::string::npos) continue;

        std::string key = trim_copy(line.substr(0, delim));
        std::string value = trim_copy(line.substr(delim + 1));

        if (key == "reference") config.reference_fasta = value;
        else if (key == "read1") config.read1_fastq = value;
        else if (key == "read2") config.read2_fastq = value;
        else if (key == "output") config.output_bam = value;
        else if (key == "metrics") config.output_metrics = value;
        else if (key == "threads") config.cpu_threads = std::atoi(value.c_str());
        else if (key == "gpu_id") config.gpu_device_id = std::atoi(value.c_str());
        else if (key == "batch_size") config.batch_size = static_cast<size_t>(std::stoul(value));
        else if (key == "kmer_size") config.kmer_size = static_cast<size_t>(std::stoul(value));
        else if (key == "min_mapq") config.min_mapping_quality = std::atoi(value.c_str());
        else if (key == "log_file") config.log_file = value;
        else if (key == "use_gpu") config.use_gpu = !(value == "0" || value == "false");
    }

    return true;
}
} // namespace

void print_usage(const char* program_name) {
    std::cout << "WinAlign-GPU v" << VERSION << "\n";
    std::cout << "Windows-Native GPU-Accelerated Genomic Alignment Pipeline\n\n";
    std::cout << "Usage: " << program_name << " [options]\n\n";
    std::cout << "Required arguments:\n";
    std::cout << "  -r, --reference FILE    Reference genome FASTA file\n";
    std::cout << "  -1, --read1 FILE        Forward reads (FASTQ/FASTQ.gz)\n";
    std::cout << "  -o, --output FILE       Output BAM file\n\n";
    std::cout << "Optional arguments:\n";
    std::cout << "  -2, --read2 FILE        Reverse reads for paired-end (FASTQ/FASTQ.gz)\n";
    std::cout << "  -t, --threads INT       CPU threads for I/O (default: 4)\n";
    std::cout << "  --gpu-id INT            GPU device ID (default: 0)\n";
    std::cout << "  --batch-size INT        Batch size for GPU processing (default: 10000)\n";
    std::cout << "  --kmer-size INT         K-mer size for seeding (default: 19)\n";
    std::cout << "  --min-mapq INT          Minimum mapping quality (default: 0)\n";
    std::cout << "  --config FILE           Load key=value configuration file\n";
    std::cout << "  --cpu-only              Disable GPU acceleration\n";
    std::cout << "  --log-file FILE         Persist logs to FILE\n";
    std::cout << "  --no-duplicates         Disable duplicate marking\n";
    std::cout << "  --no-sort               Disable coordinate sorting\n";
    std::cout << "  --no-index              Disable BAM index generation\n";
    std::cout << "  -h, --help              Show this help message\n";
    std::cout << "  -v, --version           Show version information\n\n";
    std::cout << "Example:\n";
    std::cout << "  " << program_name << " -r ref.fasta -1 read1.fq.gz -2 read2.fq.gz -o output.bam\n";
}

void print_version() {
    std::cout << "WinAlign-GPU version " << VERSION << "\n";
    std::cout << "Built for Windows with CUDA support\n";
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    PipelineConfig config;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string config_file;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_file = argv[++i];
        }
    }

    if (!config_file.empty()) {
        std::string error;
        if (!load_config_file(config_file, config, error)) {
            std::cerr << error << "\n";
            return 1;
        }
    }

    // Simple argument parsing (TODO: use a proper argument parser library)
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        else if (arg == "-v" || arg == "--version") {
            print_version();
            return 0;
        }
        else if (arg == "-r" || arg == "--reference") {
            if (i + 1 < argc) {
                config.reference_fasta = argv[++i];
            }
        }
        else if (arg == "-1" || arg == "--read1") {
            if (i + 1 < argc) {
                config.read1_fastq = argv[++i];
            }
        }
        else if (arg == "-2" || arg == "--read2") {
            if (i + 1 < argc) {
                config.read2_fastq = argv[++i];
            }
        }
        else if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) {
                config.output_bam = argv[++i];
            }
        }
        else if (arg == "-t" || arg == "--threads") {
            if (i + 1 < argc) {
                config.cpu_threads = std::atoi(argv[++i]);
            }
        }
        else if (arg == "--gpu-id") {
            if (i + 1 < argc) {
                config.gpu_device_id = std::atoi(argv[++i]);
            }
        }
        else if (arg == "--batch-size") {
            if (i + 1 < argc) {
                config.batch_size = std::atoi(argv[++i]);
            }
        }
        else if (arg == "--kmer-size") {
            if (i + 1 < argc) {
                config.kmer_size = std::atoi(argv[++i]);
            }
        }
        else if (arg == "--min-mapq") {
            if (i + 1 < argc) {
                config.min_mapping_quality = std::atoi(argv[++i]);
            }
        }
        else if (arg == "--no-duplicates") {
            config.mark_duplicates = false;
        }
        else if (arg == "--no-sort") {
            config.sort_output = false;
        }
        else if (arg == "--no-index") {
            config.create_index = false;
        }
        else if (arg == "--cpu-only") {
            config.use_gpu = false;
        }
        else if (arg == "--log-file") {
            if (i + 1 < argc) {
                config.log_file = argv[++i];
            }
        }
        else if (arg == "--config") {
            if (i + 1 < argc) {
                ++i; // Already processed
            }
        }
    }

    // Validate configuration
    if (!config.is_valid()) {
        std::cerr << "Error: Invalid configuration. Missing required arguments.\n";
        print_usage(argv[0]);
        return 1;
    }

    if (config.log_file.empty()) {
        std::filesystem::path out_path(config.output_bam);
        if (out_path.empty()) {
            config.log_file = "winalign.log";
        } else {
            auto log_path = out_path;
            log_path.replace_extension(".log");
            config.log_file = log_path.string();
        }
    }

    auto& logger = Logger::instance();
    logger.set_log_file(config.log_file);
    logger.info("WinAlign-GPU starting");
    logger.info(std::string("Mode: ") + (config.use_gpu ? "GPU" : "CPU-only"));
    logger.info("Reference: " + config.reference_fasta);
    logger.info("Read1: " + config.read1_fastq +
                (config.read2_fastq.empty() ? "" : " Read2: " + config.read2_fastq));

    std::cout << "WinAlign-GPU v" << VERSION << "\n";
    std::cout << "Starting alignment pipeline...\n\n";

    // Create and run pipeline
    Pipeline pipeline(config);

    // Set progress callback with enhanced display
    std::string last_stage;
    pipeline.set_progress_callback([&last_stage](double progress) {
        // Create progress bar
        const int bar_width = 50;
        int filled = static_cast<int>(progress * bar_width);

        std::cout << "\r[";
        for (int i = 0; i < bar_width; ++i) {
            if (i < filled) std::cout << "=";
            else if (i == filled) std::cout << ">";
            else std::cout << " ";
        }
        std::cout << "] " << static_cast<int>(progress * 100) << "% ";
        std::cout << std::flush;
    });

    // Initialize
    std::cout << "Initializing pipeline...\n";
    auto init_result = pipeline.initialize();
    if (!init_result.is_ok()) {
        std::cerr << "Error: Failed to initialize pipeline: " << init_result.message << "\n";
        logger.error("Initialization failed: " + init_result.message);
        return 1;
    }

    // Run
    std::cout << "Running alignment...\n";
    auto run_result = pipeline.run();
    if (!run_result.is_ok()) {
        std::cerr << "\nError: Pipeline failed: " << run_result.message << "\n";
        logger.error("Pipeline execution failed: " + run_result.message);
        return 1;
    }

    // Finalize
    std::cout << "\nFinalizing output...\n";
    auto final_result = pipeline.finalize();
    if (!final_result.is_ok()) {
        std::cerr << "Error: Failed to finalize: " << final_result.message << "\n";
        logger.error("Finalization failed: " + final_result.message);
        return 1;
    }

    // Print metrics
    const auto& metrics = pipeline.get_metrics();
    std::cout << "\nAlignment complete!\n";
    std::cout << "Total reads: " << metrics.total_reads << "\n";
    std::cout << "Aligned reads: " << metrics.aligned_reads << "\n";
    std::cout << "Alignment rate: " << (metrics.alignment_rate() * 100) << "%\n";
    std::cout << "Properly paired: " << metrics.properly_paired << "\n";
    std::cout << "Duplicates: " << metrics.duplicates << "\n";
    std::cout << "Mean quality: " << metrics.mean_quality << "\n";

    std::cout << "\nOutput written to: " << config.output_bam << "\n";

    return 0;
}
