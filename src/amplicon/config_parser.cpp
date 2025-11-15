#include "winalign/amplicon/config_parser.h"
#include <fstream>
#include <sstream>
#include <regex>
#include <iostream>

namespace winalign {
namespace amplicon {

// Simple YAML parser (minimal implementation)
// For production, consider using yaml-cpp library
namespace {

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, last - first + 1);
}

std::pair<std::string, std::string> parse_key_value(const std::string& line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) {
        return {"", ""};
    }

    std::string key = trim(line.substr(0, colon));
    std::string value = trim(line.substr(colon + 1));

    // Remove quotes
    if (!value.empty() && (value.front() == '"' || value.front() == '\'')) {
        value = value.substr(1, value.length() - 2);
    }

    return {key, value};
}

std::vector<uint64_t> parse_position_list(const std::string& str) {
    std::vector<uint64_t> positions;

    // Remove brackets
    std::string cleaned = str;
    cleaned.erase(std::remove(cleaned.begin(), cleaned.end(), '['), cleaned.end());
    cleaned.erase(std::remove(cleaned.begin(), cleaned.end(), ']'), cleaned.end());

    // Split by comma
    std::stringstream ss(cleaned);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token = trim(token);
        if (!token.empty()) {
            try {
                positions.push_back(std::stoull(token));
            } catch (...) {
                // Skip invalid entries
            }
        }
    }

    return positions;
}

} // anonymous namespace

class ConfigParser::Impl {
public:
    Result<AmpliconConfig> load(const std::string& config_path) {
        std::ifstream file(config_path);
        if (!file.is_open()) {
            return Result<AmpliconConfig>(
                ErrorCode::FILE_NOT_FOUND,
                "Cannot open config file: " + config_path
            );
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        file.close();

        return load_from_string(buffer.str());
    }

    Result<AmpliconConfig> load_from_string(const std::string& yaml_content) {
        AmpliconConfig config;

        std::istringstream stream(yaml_content);
        std::string line;

        AmpliconTarget current_target;
        bool in_target = false;
        bool in_processing = false;

        while (std::getline(stream, line)) {
            line = trim(line);

            // Skip empty lines and comments
            if (line.empty() || line[0] == '#') {
                continue;
            }

            // Check for section headers
            if (line.find("amplicon_panel:") != std::string::npos) {
                in_target = false;
                in_processing = false;
                continue;
            } else if (line.find("targets:") != std::string::npos) {
                in_target = false;
                in_processing = false;
                continue;
            } else if (line.find("processing:") != std::string::npos) {
                in_processing = true;
                in_target = false;
                continue;
            } else if (line.find("output:") != std::string::npos) {
                in_processing = false;
                in_target = false;
                continue;
            }

            // Handle list items (targets)
            if (line[0] == '-') {
                // Save previous target if exists
                if (in_target && current_target.is_valid()) {
                    config.targets.push_back(current_target);
                }

                current_target = AmpliconTarget();
                in_target = true;
                continue;
            }

            auto [key, value] = parse_key_value(line);

            if (key.empty()) continue;

            // Parse based on context
            if (in_target) {
                if (key == "amplicon_id") {
                    current_target.amplicon_id = value;
                } else if (key == "chromosome") {
                    current_target.chromosome = value;
                } else if (key == "start") {
                    current_target.start = std::stoull(value);
                } else if (key == "end") {
                    current_target.end = std::stoull(value);
                } else if (key == "primer_fwd") {
                    current_target.primer_fwd = value;
                } else if (key == "primer_rev") {
                    current_target.primer_rev = value;
                } else if (key == "snp_positions") {
                    current_target.snp_positions = parse_position_list(value);
                } else if (key == "expected_length") {
                    current_target.expected_length = std::stoul(value);
                } else {
                    // Calculate expected length if not provided
                    if (current_target.end > current_target.start) {
                        current_target.expected_length = current_target.end - current_target.start;
                    }
                }
            } else if (in_processing) {
                if (key == "collapse_reads") {
                    config.collapse_reads = (value == "true" || value == "True" || value == "1");
                } else if (key == "max_cluster_distance") {
                    config.max_cluster_distance = std::stoul(value);
                } else if (key == "min_depth") {
                    config.min_depth = std::stoul(value);
                } else if (key == "min_allele_frequency") {
                    config.min_allele_frequency = std::stof(value);
                } else if (key == "quality_threshold") {
                    config.quality_threshold = std::stoul(value);
                }
            } else {
                // Global settings
                if (key == "name") {
                    config.panel_name = value;
                } else if (key == "reference") {
                    config.reference_fasta = value;
                } else if (key == "format") {
                    // output format
                }
            }
        }

        // Save last target
        if (in_target && current_target.is_valid()) {
            config.targets.push_back(current_target);
        }

        // Validate configuration
        if (!validate(config)) {
            return Result<AmpliconConfig>(
                ErrorCode::INVALID_FORMAT,
                "Invalid configuration: missing required fields"
            );
        }

        return Result<AmpliconConfig>(config);
    }

    static bool validate(const AmpliconConfig& config) {
        // Check required fields
        if (config.reference_fasta.empty()) {
            std::cerr << "Error: reference_fasta is required\n";
            return false;
        }

        if (config.targets.empty()) {
            std::cerr << "Error: at least one amplicon target is required\n";
            return false;
        }

        // Validate each target
        for (const auto& target : config.targets) {
            if (!target.is_valid()) {
                std::cerr << "Error: invalid target: " << target.amplicon_id << "\n";
                return false;
            }
        }

        return true;
    }

    static void create_example_config(const std::string& output_path) {
        std::ofstream out(output_path);
        if (!out.is_open()) {
            std::cerr << "Cannot create example config: " << output_path << "\n";
            return;
        }

        out << "# WinAlign-Amplicon Configuration Example\n";
        out << "# YAML format for amplicon sequencing panel definition\n\n";

        out << "amplicon_panel:\n";
        out << "  name: \"Example Cannabis Genotyping Panel\"\n";
        out << "  reference: \"cannabis_sativa_cs10.fasta\"\n\n";

        out << "targets:\n";
        out << "  - amplicon_id: \"MARKER_001\"\n";
        out << "    chromosome: \"chr1\"\n";
        out << "    start: 12345\n";
        out << "    end: 12545\n";
        out << "    primer_fwd: \"ACGTACGTACGTACGT\"\n";
        out << "    primer_rev: \"GCTAGCTAGCTAGCTA\"\n";
        out << "    snp_positions: [12400, 12450]\n\n";

        out << "  - amplicon_id: \"MARKER_002\"\n";
        out << "    chromosome: \"chr2\"\n";
        out << "    start: 54321\n";
        out << "    end: 54521\n";
        out << "    primer_fwd: \"TGCATGCATGCATGCA\"\n";
        out << "    primer_rev: \"ATCGATCGATCGATCG\"\n";
        out << "    snp_positions: [54400]\n\n";

        out << "processing:\n";
        out << "  collapse_reads: true\n";
        out << "  max_cluster_distance: 2\n";
        out << "  min_depth: 100\n";
        out << "  min_allele_frequency: 0.01\n";
        out << "  quality_threshold: 20\n\n";

        out << "output:\n";
        out << "  format: \"vcf\"\n";
        out << "  include_depth: true\n";
        out << "  include_allele_frequencies: true\n";

        out.close();
        std::cout << "Created example config: " << output_path << "\n";
    }
};

// Public interface
ConfigParser::ConfigParser()
    : pimpl_(std::make_unique<Impl>()) {}

ConfigParser::~ConfigParser() = default;

Result<AmpliconConfig> ConfigParser::load(const std::string& config_path) {
    return pimpl_->load(config_path);
}

Result<AmpliconConfig> ConfigParser::load_from_string(const std::string& yaml_content) {
    return pimpl_->load_from_string(yaml_content);
}

bool ConfigParser::validate(const AmpliconConfig& config) {
    return Impl::validate(config);
}

void ConfigParser::create_example_config(const std::string& output_path) {
    Impl::create_example_config(output_path);
}

} // namespace amplicon
} // namespace winalign
