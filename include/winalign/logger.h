#ifndef WINALIGN_LOGGER_H
#define WINALIGN_LOGGER_H

#include <fstream>
#include <mutex>
#include <string>

namespace winalign {

/**
 * @brief Minimal thread-safe logger that writes to a persisted log file.
 *
 * The logger is intentionally lightweight so it can be used across CPU/GPU
 * components without pulling in an external dependency.
 */
class Logger {
public:
    /**
     * @brief Get singleton instance.
     */
    static Logger& instance();

    /**
     * @brief Configure output file. Creates parent directories on demand.
     * @param path Absolute or relative path to log file.
     * @param append Whether to append to the log (default true).
     */
    void set_log_file(const std::string& path, bool append = true);

    /**
     * @brief Log informational message.
     */
    void info(const std::string& message);

    /**
     * @brief Log warning message (still non-fatal).
     */
    void warn(const std::string& message);

    /**
     * @brief Log error message.
     */
    void error(const std::string& message);

    /**
     * @brief Retrieve current log file path.
     */
    std::string log_path() const;

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void write(const char* level, const std::string& message);

    mutable std::mutex mutex_;
    std::ofstream stream_;
    std::string path_;
    bool enabled_ = false;
};

} // namespace winalign

#endif // WINALIGN_LOGGER_H
