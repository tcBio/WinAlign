#ifndef WINALIGN_AMPLICON_CONFIG_PARSER_H
#define WINALIGN_AMPLICON_CONFIG_PARSER_H

#include "amplicon_types.h"
#include "winalign/common.h"
#include <string>

namespace winalign {
namespace amplicon {

/**
 * Configuration Parser
 *
 * Parses YAML configuration files for amplicon panels.
 * Supports both simple key-value format and structured YAML.
 */
class ConfigParser {
public:
    ConfigParser();
    ~ConfigParser();

    /**
     * Load configuration from YAML file
     *
     * @param config_path Path to YAML configuration file
     * @return Result with parsed AmpliconConfig
     */
    Result<AmpliconConfig> load(const std::string& config_path);

    /**
     * Load configuration from string (for testing)
     */
    Result<AmpliconConfig> load_from_string(const std::string& yaml_content);

    /**
     * Validate configuration
     */
    static bool validate(const AmpliconConfig& config);

    /**
     * Create example configuration file
     */
    static void create_example_config(const std::string& output_path);

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_CONFIG_PARSER_H
