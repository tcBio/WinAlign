#ifndef WINALIGN_COMMON_H
#define WINALIGN_COMMON_H

#include <cstdint>
#include <string>
#include <vector>

namespace winalign {

// Version information
constexpr const char* VERSION = "0.1.0";
constexpr int VERSION_MAJOR = 0;
constexpr int VERSION_MINOR = 1;
constexpr int VERSION_PATCH = 0;

// Common types
using ReadID = uint64_t;
using Position = uint64_t;
using QualityScore = uint8_t;

// Constants
constexpr size_t DEFAULT_KMER_SIZE = 19;
// Default GPU batch size. Larger batches improve GPU utilization
// at the cost of higher memory usage.
constexpr size_t DEFAULT_BATCH_SIZE = 60000;
constexpr size_t MAX_READ_LENGTH = 1024;
constexpr size_t MAX_CIGAR_LENGTH = 2048;

// Quality score encoding
constexpr QualityScore PHRED_OFFSET = 33;
constexpr QualityScore MIN_QUALITY = 0;
constexpr QualityScore MAX_QUALITY = 60;

// Alignment scores
struct AlignmentScores {
    int match = 1;
    int mismatch = -4;
    int gap_open = -6;
    int gap_extend = -1;
};

// Read structure
struct Read {
    ReadID id;
    std::string name;
    std::string sequence;
    std::string quality;
    bool is_reverse;

    Read() : id(0), is_reverse(false) {}

    size_t length() const { return sequence.length(); }
    bool empty() const { return sequence.empty(); }
};

// Read pair
struct ReadPair {
    Read read1;
    Read read2;

    bool empty() const { return read1.empty() && read2.empty(); }
};

// Alignment result
struct Alignment {
    ReadID read_id;
    std::string reference_name;
    Position position;
    uint16_t mapping_quality;
    uint16_t flag;
    std::string cigar;
    int alignment_score;
    bool is_primary;

    Alignment() : read_id(0), position(0), mapping_quality(0),
                  flag(0), alignment_score(0), is_primary(true) {}
};

// SAM/BAM flags
namespace sam_flags {
    constexpr uint16_t PAIRED = 0x1;
    constexpr uint16_t PROPER_PAIR = 0x2;
    constexpr uint16_t UNMAPPED = 0x4;
    constexpr uint16_t MATE_UNMAPPED = 0x8;
    constexpr uint16_t REVERSE = 0x10;
    constexpr uint16_t MATE_REVERSE = 0x20;
    constexpr uint16_t READ1 = 0x40;
    constexpr uint16_t READ2 = 0x80;
    constexpr uint16_t SECONDARY = 0x100;
    constexpr uint16_t QCFAIL = 0x200;
    constexpr uint16_t DUPLICATE = 0x400;
    constexpr uint16_t SUPPLEMENTARY = 0x800;
}

// Error codes
enum class ErrorCode {
    SUCCESS = 0,
    FILE_NOT_FOUND,
    INVALID_FORMAT,
    CUDA_ERROR,
    OUT_OF_MEMORY,
    INVALID_ARGUMENT,
    RUNTIME_ERROR
};

// Result type
template<typename T>
struct Result {
    T value;
    ErrorCode error;
    std::string message;

    Result() : error(ErrorCode::SUCCESS) {}
    Result(T val) : value(std::move(val)), error(ErrorCode::SUCCESS) {}
    Result(ErrorCode err, const std::string& msg) : error(err), message(msg) {}

    bool is_ok() const { return error == ErrorCode::SUCCESS; }
    bool is_error() const { return error != ErrorCode::SUCCESS; }
};

} // namespace winalign

#endif // WINALIGN_COMMON_H
