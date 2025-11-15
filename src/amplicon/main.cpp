#include "winalign/amplicon/amplicon_pipeline.h"
#include "winalign/amplicon/config_parser.h"
#include <iostream>
#include <iomanip>
#include <cstring>

using namespace winalign;
using namespace winalign::amplicon;

void print_usage(const char* program_name) {
    std::cout << "WinAlign-Amplicon v" << VERSION << "\n";
    std::cout << "GPU-Accelerated Amplicon Sequencing Analysis\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << program_name << " [options]\n\n";
    std::cout << "Required Options:\n";
    std::cout << "  -c, --config FILE      Amplicon panel configuration (YAML)\n";
    std::cout << "  -i, --input FILE       Input FASTQ file (.fastq or .fastq.gz)\n";
    std::cout << "  -o, --output FILE      Output VCF file\n\n";
    std::cout << "Optional:\n";
    std::cout << "  -s, --stats FILE       QC statistics output (JSON)\n";
    std::cout << "  -g, --gpu-id INT       GPU device ID (default: 0)\n";
    std::cout << "  -b, --batch-size INT   Batch size (default: 10000)\n";
    std::cout << "  -h, --help             Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  # Basic usage\n";
    std::cout << "  " << program_name << " -c panel.yaml -i reads.fastq.gz -o genotypes.vcf\n\n";
    std::cout << "  # With statistics\n";
    std::cout << "  " << program_name << " -c panel.yaml -i reads.fq -o out.vcf -s qc.json\n\n";
    std::cout << "  # Create example config\n";
    std::cout << "  " << program_name << " --create-config example.yaml\n\n";
}

struct CommandLineArgs {
    std::string config_file;
    std::string input_fastq;
    std::string output_vcf;
    std::string stats_file;
    uint32_t gpu_id = 0;
    uint32_t batch_size = 10000;
    bool create_config = false;
    std::string create_config_path;
};

CommandLineArgs parse_args(int argc, char* argv[]) {
    CommandLineArgs args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            exit(0);
        } else if (arg == "--create-config") {
            args.create_config = true;
            if (i + 1 < argc) {
                args.create_config_path = argv[++i];
            } else {
                args.create_config_path = "example_amplicon.yaml";
            }
        } else if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            args.config_file = argv[++i];
        } else if ((arg == "-i" || arg == "--input") && i + 1 < argc) {
            args.input_fastq = argv[++i];
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            args.output_vcf = argv[++i];
        } else if ((arg == "-s" || arg == "--stats") && i + 1 < argc) {
            args.stats_file = argv[++i];
        } else if ((arg == "-g" || arg == "--gpu-id") && i + 1 < argc) {
            args.gpu_id = std::stoul(argv[++i]);
        } else if ((arg == "-b" || arg == "--batch-size") && i + 1 < argc) {
            args.batch_size = std::stoul(argv[++i]);
        }
    }

    return args;
}

void print_progress_bar(double progress, const std::string& message) {
    const int bar_width = 50;
    int pos = static_cast<int>(bar_width * progress);

    std::cout << "\r[";
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << std::fixed << std::setprecision(1)
              << (progress * 100.0) << "% - " << message;
    std::cout.flush();

    if (progress >= 1.0) {
        std::cout << "\n";
    }
}

int main(int argc, char* argv[]) {
    std::cout << "=== WinAlign-Amplicon v" << VERSION << " ===\n\n";

    // Parse arguments
    CommandLineArgs args = parse_args(argc, argv);

    // Handle create-config mode
    if (args.create_config) {
        ConfigParser::create_example_config(args.create_config_path);
        return 0;
    }

    // Validate required arguments
    if (args.config_file.empty() || args.input_fastq.empty() || args.output_vcf.empty()) {
        std::cerr << "Error: Missing required arguments\n\n";
        print_usage(argv[0]);
        return 1;
    }

    // Load configuration
    std::cout << "[1/5] Loading configuration: " << args.config_file << "\n";
    ConfigParser parser;
    auto config_result = parser.load(args.config_file);

    if (!config_result.is_ok()) {
        std::cerr << "Error loading config: " << config_result.message << "\n";
        return 1;
    }

    AmpliconConfig config = config_result.value;

    // Override config with command-line arguments
    config.input_fastq = args.input_fastq;
    config.output_vcf = args.output_vcf;
    config.output_stats = args.stats_file;
    config.gpu_device_id = args.gpu_id;
    config.batch_size = args.batch_size;

    // Print configuration summary
    std::cout << "  Panel: " << config.panel_name << "\n";
    std::cout << "  Targets: " << config.targets.size() << " amplicons\n";
    std::cout << "  Reference: " << config.reference_fasta << "\n";
    std::cout << "  Input: " << config.input_fastq << "\n";
    std::cout << "  Output: " << config.output_vcf << "\n";
    std::cout << "\n";

    // Create and initialize pipeline
    std::cout << "[2/5] Initializing pipeline...\n";
    AmpliconPipeline pipeline(config);

    // Set progress callback
    pipeline.set_progress_callback(
        [](double progress, const std::string& message) {
            print_progress_bar(progress, message);
        }
    );

    // Initialize
    auto init_result = pipeline.initialize();
    if (!init_result.is_ok()) {
        std::cerr << "Error initializing pipeline: " << init_result.message << "\n";
        return 1;
    }

    // Run pipeline
    std::cout << "[3/5] Processing amplicon data...\n";
    auto run_result = pipeline.run();
    if (!run_result.is_ok()) {
        std::cerr << "Error running pipeline: " << run_result.message << "\n";
        return 1;
    }

    // Finalize
    std::cout << "[4/5] Finalizing and writing output...\n";
    auto final_result = pipeline.finalize();
    if (!final_result.is_ok()) {
        std::cerr << "Error finalizing: " << final_result.message << "\n";
        return 1;
    }

    // Print statistics
    std::cout << "[5/5] Complete!\n\n";
    auto stats = pipeline.get_stats();

    std::cout << "=== Pipeline Statistics ===\n";
    std::cout << "  Total reads:          " << stats.total_reads << "\n";
    std::cout << "  Collapsed clusters:   " << stats.collapsed_clusters << "\n";
    std::cout << "  Collapse ratio:       " << std::fixed << std::setprecision(1)
              << stats.collapse_ratio << "x\n";
    std::cout << "  Assigned clusters:    " << stats.assigned_clusters << "\n";
    std::cout << "  Assignment rate:      " << std::fixed << std::setprecision(1)
              << (stats.assignment_rate * 100.0) << "%\n";
    std::cout << "  Aligned clusters:     " << stats.aligned_clusters << "\n";
    std::cout << "  Alignment rate:       " << std::fixed << std::setprecision(1)
              << (stats.alignment_rate * 100.0) << "%\n";
    std::cout << "  Variants called:      " << stats.variants_called << "\n";
    std::cout << "\n";

    std::cout << "Output files:\n";
    std::cout << "  VCF: " << config.output_vcf << "\n";
    if (!config.output_stats.empty()) {
        std::cout << "  Statistics: " << config.output_stats << "\n";
    }
    std::cout << "\n";

    return 0;
}
