#include "winalign/reference_loader.h"
#include "winalign/fm_index.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>

namespace winalign {

class ReferenceLoader::Impl {
public:
    Impl(const std::string& fasta_path) : fasta_path_(fasta_path), fm_index_(nullptr) {}

    Result<bool> load() {
        std::ifstream file(fasta_path_);
        if (!file.is_open()) {
            return Result<bool>(ErrorCode::FILE_NOT_FOUND,
                              "Failed to open FASTA file: " + fasta_path_);
        }

        sequences_.clear();
        concatenated_sequence_.clear();
        chrom_offsets_.clear();
        sequence_order_.clear();

        std::string line;
        std::string current_name;
        std::string current_sequence;

        while (std::getline(file, line)) {
            // Remove trailing whitespace
            while (!line.empty() && std::isspace(line.back())) {
                line.pop_back();
            }

            if (line.empty()) continue;

            if (line[0] == '>') {
                // Header line - save previous sequence if any
                if (!current_name.empty()) {
                    save_sequence(current_name, current_sequence);
                    current_sequence.clear();
                }

                // Extract sequence name (everything after '>' until first whitespace)
                current_name = line.substr(1);
                size_t space_pos = current_name.find(' ');
                if (space_pos != std::string::npos) {
                    current_name = current_name.substr(0, space_pos);
                }
            } else {
                // Sequence line
                current_sequence += line;
            }
        }

        // Save last sequence
        if (!current_name.empty()) {
            save_sequence(current_name, current_sequence);
        }

        file.close();

        if (sequences_.empty()) {
            return Result<bool>(ErrorCode::INVALID_FORMAT,
                              "No sequences found in FASTA file");
        }

        std::cout << "Loaded " << sequences_.size() << " sequences ("
                  << total_length() << " bp total)\n";

        return Result<bool>(true);
    }

    Result<bool> build_index() {
        if (sequences_.empty()) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR,
                              "No sequences loaded. Call load() first.");
        }

        concatenated_sequence_.clear();
        chrom_offsets_.clear();

        auto append_sequence = [&](const ReferenceSequence& seq) {
            ChromosomeOffset offset;
            offset.name = seq.name;
            offset.start = concatenated_sequence_.size();
            offset.length = seq.length;
            chrom_offsets_.push_back(offset);
            concatenated_sequence_ += seq.sequence;
        };

        if (!sequence_order_.empty()) {
            for (const auto& name : sequence_order_) {
                auto it = sequences_.find(name);
                if (it != sequences_.end()) {
                    append_sequence(it->second);
                }
            }
        } else {
            for (const auto& [name, seq] : sequences_) {
                append_sequence(seq);
            }
        }

        std::cout << "Building FM-index for " << concatenated_sequence_.length()
                  << " bp...\n";

        // Build FM-index
        fm_index_ = std::make_unique<FMIndex>();
        auto result = fm_index_->build(concatenated_sequence_.c_str(),
                                       concatenated_sequence_.length());

        if (!result.is_ok()) {
            fm_index_.reset();
            return result;
        }

        indexed_ = true;
        std::cout << "FM-index built successfully\n";

        return Result<bool>(true);
    }

    Result<bool> save_index(const std::string& index_path) {
        if (!indexed_ || !fm_index_) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR,
                              "Index not built. Call build_index() first.");
        }

        return fm_index_->save(index_path);
    }

    Result<bool> load_index(const std::string& index_path) {
        fm_index_ = std::make_unique<FMIndex>();
        auto result = fm_index_->load(index_path);

        if (result.is_ok()) {
            indexed_ = true;
        } else {
            fm_index_.reset();
        }

        return result;
    }

    const ReferenceSequence* get_sequence(const std::string& name) const {
        auto it = sequences_.find(name);
        return (it != sequences_.end()) ? &it->second : nullptr;
    }

    const std::map<std::string, ReferenceSequence>& get_sequences() const {
        return sequences_;
    }

    uint64_t total_length() const {
        uint64_t total = 0;
        for (const auto& [name, seq] : sequences_) {
            total += seq.length;
        }
        return total;
    }

    size_t num_sequences() const {
        return sequences_.size();
    }

    bool is_indexed() const {
        return indexed_;
    }

    const void* get_fm_index_data() const {
        return fm_index_ ? fm_index_->get_bwt() : nullptr;
    }

    size_t get_fm_index_size() const {
        return fm_index_ ? fm_index_->get_data_size() : 0;
    }

    const FMIndex* get_fm_index() const {
        return fm_index_.get();
    }

    const std::string& get_concatenated_sequence() const {
        return concatenated_sequence_;
    }

    bool map_global_position(uint64_t global_pos,
                             std::string& contig,
                             uint64_t& local_offset) const {
        if (chrom_offsets_.empty() || global_pos >= concatenated_sequence_.size()) {
            return false;
        }

        for (const auto& entry : chrom_offsets_) {
            uint64_t start = entry.start;
            uint64_t end = entry.start + entry.length;
            if (global_pos >= start && global_pos < end) {
                contig = entry.name;
                local_offset = global_pos - start;
                return true;
            }
        }
        return false;
    }

    const std::vector<ChromosomeOffset>& get_chrom_offsets() const {
        return chrom_offsets_;
    }

    std::string get_subsequence(uint64_t global_start, uint32_t length) const {
        if (global_start >= concatenated_sequence_.size()) {
            return {};
        }
        uint64_t available = concatenated_sequence_.size() - global_start;
        uint32_t span = static_cast<uint32_t>(std::min<uint64_t>(available, length));
        return concatenated_sequence_.substr(global_start, span);
    }

private:
    void save_sequence(const std::string& name, const std::string& sequence) {
        ReferenceSequence ref_seq;
        ref_seq.name = name;
        ref_seq.sequence = sequence;
        ref_seq.length = sequence.length();

        if (!sequences_.count(name)) {
            sequence_order_.push_back(name);
        }

        sequences_[name] = std::move(ref_seq);
    }

    std::string fasta_path_;
    std::map<std::string, ReferenceSequence> sequences_;
    std::string concatenated_sequence_; // All sequences concatenated
    std::unique_ptr<FMIndex> fm_index_;
    bool indexed_ = false;
    std::vector<ChromosomeOffset> chrom_offsets_;
    std::vector<std::string> sequence_order_;
};

ReferenceLoader::ReferenceLoader(const std::string& fasta_path)
    : pimpl_(std::make_unique<Impl>(fasta_path)) {}

ReferenceLoader::~ReferenceLoader() = default;

Result<bool> ReferenceLoader::load() { return pimpl_->load(); }
Result<bool> ReferenceLoader::build_index() { return pimpl_->build_index(); }
Result<bool> ReferenceLoader::save_index(const std::string& index_path) {
    return pimpl_->save_index(index_path);
}
Result<bool> ReferenceLoader::load_index(const std::string& index_path) {
    return pimpl_->load_index(index_path);
}
const ReferenceSequence* ReferenceLoader::get_sequence(const std::string& name) const {
    return pimpl_->get_sequence(name);
}
const std::map<std::string, ReferenceSequence>& ReferenceLoader::get_sequences() const {
    return pimpl_->get_sequences();
}
uint64_t ReferenceLoader::total_length() const { return pimpl_->total_length(); }
size_t ReferenceLoader::num_sequences() const { return pimpl_->num_sequences(); }
bool ReferenceLoader::is_indexed() const { return pimpl_->is_indexed(); }
const void* ReferenceLoader::get_fm_index_data() const {
    return pimpl_->get_fm_index_data();
}
size_t ReferenceLoader::get_fm_index_size() const {
    return pimpl_->get_fm_index_size();
}
const FMIndex* ReferenceLoader::get_fm_index() const {
    return pimpl_->get_fm_index();
}
const std::string& ReferenceLoader::concatenated_sequence() const {
    return pimpl_->get_concatenated_sequence();
}
bool ReferenceLoader::map_global_position(uint64_t global_pos,
                                          std::string& contig,
                                          uint64_t& local_offset) const {
    return pimpl_->map_global_position(global_pos, contig, local_offset);
}
const std::vector<ChromosomeOffset>& ReferenceLoader::chromosome_offsets() const {
    return pimpl_->get_chrom_offsets();
}
std::string ReferenceLoader::get_subsequence(uint64_t global_start, uint32_t length) const {
    return pimpl_->get_subsequence(global_start, length);
}

} // namespace winalign
