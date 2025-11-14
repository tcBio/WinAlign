#include "winalign/fm_index.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <numeric>

namespace winalign {

// Helper functions for BWT construction
namespace {

// Nucleotide to index mapping (A=0, C=1, G=2, T=3, N=4)
inline uint8_t char_to_index(char c) {
    switch (c) {
        case 'A': case 'a': return 0;
        case 'C': case 'c': return 1;
        case 'G': case 'g': return 2;
        case 'T': case 't': return 3;
        default: return 4; // N or other
    }
}

inline char index_to_char(uint8_t idx) {
    const char chars[] = {'A', 'C', 'G', 'T', 'N'};
    return idx < 5 ? chars[idx] : 'N';
}

// Simple suffix array construction using induced sorting (SA-IS algorithm simplified)
void build_suffix_array(const char* text, size_t length, std::vector<uint64_t>& sa) {
    sa.resize(length);

    // For proof of concept, use a simpler O(n log n) approach
    // In production, use libdivsufsort or similar optimized library

    std::vector<std::pair<std::string_view, uint64_t>> suffixes;
    suffixes.reserve(length);

    for (size_t i = 0; i < length; ++i) {
        suffixes.emplace_back(std::string_view(text + i, length - i), i);
    }

    // Sort suffixes lexicographically
    std::sort(suffixes.begin(), suffixes.end(),
              [](const auto& a, const auto& b) {
                  return a.first < b.first;
              });

    // Extract suffix array
    for (size_t i = 0; i < length; ++i) {
        sa[i] = suffixes[i].second;
    }
}

// Build BWT from suffix array
void build_bwt(const char* text, size_t length, const std::vector<uint64_t>& sa,
               std::vector<uint8_t>& bwt) {
    bwt.resize(length);

    for (size_t i = 0; i < length; ++i) {
        if (sa[i] == 0) {
            // Special case: wrap around to last character
            bwt[i] = char_to_index(text[length - 1]);
        } else {
            bwt[i] = char_to_index(text[sa[i] - 1]);
        }
    }
}

} // anonymous namespace

// FMIndex implementation
class FMIndex::Impl {
public:
    Impl() : length_(0), built_(false), occ_interval_(128) {}

    Result<bool> build(const char* sequence, size_t length) {
        if (!sequence || length == 0) {
            return Result<bool>(ErrorCode::INVALID_ARGUMENT, "Invalid sequence");
        }

        length_ = length;

        // Step 1: Build suffix array
        std::vector<uint64_t> suffix_array;
        build_suffix_array(sequence, length, suffix_array);

        // Step 2: Build BWT from suffix array
        build_bwt(sequence, length, suffix_array, bwt_);

        // Step 3: Build C table (cumulative character counts)
        build_c_table();

        // Step 4: Build occurrence table (for fast rank queries)
        build_occ_table();

        built_ = true;
        return Result<bool>(true);
    }

    size_t search(const char* pattern, size_t pattern_len, std::vector<Position>& matches) {
        if (!built_ || !pattern || pattern_len == 0) {
            return 0;
        }

        matches.clear();

        // Backward search algorithm
        uint64_t sp = 0;          // Start pointer
        uint64_t ep = length_ - 1; // End pointer

        // Process pattern from right to left
        for (int i = pattern_len - 1; i >= 0; --i) {
            uint8_t c = char_to_index(pattern[i]);

            // Update range using LF-mapping
            sp = c_table_[c] + rank(c, sp - 1);
            ep = c_table_[c] + rank(c, ep) - 1;

            if (sp > ep) {
                return 0; // No matches
            }
        }

        // Extract positions from suffix array range
        // For now, just return count (positions would require storing SA)
        size_t count = ep - sp + 1;

        // TODO: Store suffix array or use sampling for position recovery
        // For now, return approximate positions
        for (uint64_t i = sp; i <= ep && matches.size() < 1000; ++i) {
            matches.push_back(i); // These would be actual genome positions
        }

        return count;
    }

    size_t count(const char* pattern, size_t pattern_len) {
        if (!built_ || !pattern || pattern_len == 0) {
            return 0;
        }

        uint64_t sp = 0;
        uint64_t ep = length_ - 1;

        for (int i = pattern_len - 1; i >= 0; --i) {
            uint8_t c = char_to_index(pattern[i]);
            sp = c_table_[c] + rank(c, sp - 1);
            ep = c_table_[c] + rank(c, ep) - 1;

            if (sp > ep) {
                return 0;
            }
        }

        return ep - sp + 1;
    }

    const uint8_t* get_bwt() const { return bwt_.data(); }
    const uint64_t* get_c_table() const { return c_table_; }
    const uint64_t* get_occ_table() const { return occ_table_.data(); }

    size_t get_data_size() const {
        return bwt_.size() + sizeof(c_table_) + occ_table_.size() * sizeof(uint64_t);
    }

    size_t get_length() const { return length_; }
    bool is_built() const { return built_; }

    Result<bool> save(const std::string& filename) {
        std::ofstream out(filename, std::ios::binary);
        if (!out.is_open()) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR, "Failed to open file for writing");
        }

        // Write header
        out.write(reinterpret_cast<const char*>(&length_), sizeof(length_));
        out.write(reinterpret_cast<const char*>(&occ_interval_), sizeof(occ_interval_));

        // Write BWT
        out.write(reinterpret_cast<const char*>(bwt_.data()), bwt_.size());

        // Write C table
        out.write(reinterpret_cast<const char*>(c_table_), sizeof(c_table_));

        // Write occurrence table
        size_t occ_size = occ_table_.size();
        out.write(reinterpret_cast<const char*>(&occ_size), sizeof(occ_size));
        out.write(reinterpret_cast<const char*>(occ_table_.data()),
                  occ_size * sizeof(uint64_t));

        out.close();
        return Result<bool>(true);
    }

    Result<bool> load(const std::string& filename) {
        std::ifstream in(filename, std::ios::binary);
        if (!in.is_open()) {
            return Result<bool>(ErrorCode::FILE_NOT_FOUND, "Failed to open index file");
        }

        // Read header
        in.read(reinterpret_cast<char*>(&length_), sizeof(length_));
        in.read(reinterpret_cast<char*>(&occ_interval_), sizeof(occ_interval_));

        // Read BWT
        bwt_.resize(length_);
        in.read(reinterpret_cast<char*>(bwt_.data()), length_);

        // Read C table
        in.read(reinterpret_cast<char*>(c_table_), sizeof(c_table_));

        // Read occurrence table
        size_t occ_size;
        in.read(reinterpret_cast<char*>(&occ_size), sizeof(occ_size));
        occ_table_.resize(occ_size);
        in.read(reinterpret_cast<char*>(occ_table_.data()),
                occ_size * sizeof(uint64_t));

        in.close();
        built_ = true;
        return Result<bool>(true);
    }

private:
    // Build C table (cumulative counts of characters)
    void build_c_table() {
        // Count occurrences of each character
        uint64_t counts[5] = {0};
        for (uint8_t c : bwt_) {
            if (c < 5) counts[c]++;
        }

        // Build cumulative table
        c_table_[0] = 0;
        for (int i = 1; i < 5; ++i) {
            c_table_[i] = c_table_[i-1] + counts[i-1];
        }
    }

    // Build occurrence table for fast rank queries
    void build_occ_table() {
        size_t num_checkpoints = (length_ + occ_interval_ - 1) / occ_interval_;
        occ_table_.resize(num_checkpoints * 5);

        uint64_t counts[5] = {0};

        for (size_t i = 0; i < length_; ++i) {
            uint8_t c = bwt_[i];
            if (c < 5) counts[c]++;

            // Store checkpoint
            if ((i + 1) % occ_interval_ == 0) {
                size_t checkpoint = i / occ_interval_;
                for (int j = 0; j < 5; ++j) {
                    occ_table_[checkpoint * 5 + j] = counts[j];
                }
            }
        }
    }

    // Rank query: count occurrences of character c up to position i
    uint64_t rank(uint8_t c, uint64_t i) const {
        if (c >= 5 || i >= length_) return 0;
        if (i == static_cast<uint64_t>(-1)) return 0;

        // Get checkpoint
        uint64_t checkpoint = i / occ_interval_;
        uint64_t count = (checkpoint > 0) ? occ_table_[(checkpoint - 1) * 5 + c] : 0;

        // Count from checkpoint to position
        uint64_t start = checkpoint * occ_interval_;
        for (uint64_t j = start; j <= i; ++j) {
            if (bwt_[j] == c) count++;
        }

        return count;
    }

    size_t length_;
    bool built_;
    uint32_t occ_interval_; // Checkpoint interval for occurrence table

    std::vector<uint8_t> bwt_;        // Burrows-Wheeler Transform
    uint64_t c_table_[5];             // Cumulative character counts (A,C,G,T,N)
    std::vector<uint64_t> occ_table_; // Occurrence table for rank queries
};

// FMIndex public interface
FMIndex::FMIndex() : pimpl_(std::make_unique<Impl>()) {}
FMIndex::~FMIndex() = default;
FMIndex::FMIndex(FMIndex&&) noexcept = default;
FMIndex& FMIndex::operator=(FMIndex&&) noexcept = default;

Result<bool> FMIndex::build(const char* sequence, size_t length) {
    return pimpl_->build(sequence, length);
}

size_t FMIndex::search(const char* pattern, size_t length, std::vector<Position>& matches) {
    return pimpl_->search(pattern, length, matches);
}

size_t FMIndex::count(const char* pattern, size_t length) {
    return pimpl_->count(pattern, length);
}

const uint8_t* FMIndex::get_bwt() const { return pimpl_->get_bwt(); }
const uint64_t* FMIndex::get_c_table() const { return pimpl_->get_c_table(); }
const uint64_t* FMIndex::get_occ_table() const { return pimpl_->get_occ_table(); }
size_t FMIndex::get_data_size() const { return pimpl_->get_data_size(); }
size_t FMIndex::get_length() const { return pimpl_->get_length(); }
bool FMIndex::is_built() const { return pimpl_->is_built(); }

Result<bool> FMIndex::save(const std::string& filename) {
    return pimpl_->save(filename);
}

Result<bool> FMIndex::load(const std::string& filename) {
    return pimpl_->load(filename);
}

} // namespace winalign
