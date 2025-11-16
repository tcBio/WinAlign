#include "winalign/logger.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

namespace winalign {

namespace {
std::string timestamp() {
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    auto time = clock::to_time_t(now);
    std::tm tm_snapshot;
#if defined(_WIN32)
    localtime_s(&tm_snapshot, &time);
#else
    localtime_r(&time, &tm_snapshot);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_snapshot, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}
} // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::set_log_file(const std::string& path, bool append) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = path;

    if (path_.empty()) {
        enabled_ = false;
        stream_.close();
        return;
    }

    try {
        std::filesystem::path fs_path(path_);
        if (fs_path.has_parent_path()) {
            std::filesystem::create_directories(fs_path.parent_path());
        }

        if (stream_.is_open()) {
            stream_.close();
        }

        stream_.open(path_, append ? std::ios::app : std::ios::trunc);
        enabled_ = stream_.is_open();
    } catch (const std::exception& ex) {
        enabled_ = false;
        std::cerr << "Logger failed to open file " << path_
                  << ": " << ex.what() << "\n";
    }
}

void Logger::info(const std::string& message) {
    write("INFO", message);
}

void Logger::warn(const std::string& message) {
    write("WARN", message);
}

void Logger::error(const std::string& message) {
    write("ERROR", message);
}

std::string Logger::log_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

void Logger::write(const char* level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string line =
        "[" + timestamp() + "] [" + level + "] " + message + "\n";

    if (enabled_ && stream_.is_open()) {
        stream_ << line;
        stream_.flush();
    }

    std::string_view level_view(level);
    if (level_view == "ERROR" || level_view == "WARN") {
        std::cerr << line;
    }
}

} // namespace winalign
