#include "winalign/reference_loader.h"

namespace winalign {

class ReferenceLoader::Impl {
public:
    Impl(const std::string& fasta_path) : fasta_path_(fasta_path) {}

    Result<bool> load() {
        // TODO: Load FASTA file
        return Result<bool>(ErrorCode::RUNTIME_ERROR, "Not implemented");
    }

    Result<bool> build_index() {
        // TODO: Build FM-index
        return Result<bool>(ErrorCode::RUNTIME_ERROR, "Not implemented");
    }

    Result<bool> save_index(const std::string& index_path) {
        // TODO: Save index to file
        return Result<bool>(ErrorCode::RUNTIME_ERROR, "Not implemented");
    }

    Result<bool> load_index(const std::string& index_path) {
        // TODO: Load index from file
        return Result<bool>(ErrorCode::RUNTIME_ERROR, "Not implemented");
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
        return nullptr; // TODO
    }

    size_t get_fm_index_size() const {
        return 0; // TODO
    }

private:
    std::string fasta_path_;
    std::map<std::string, ReferenceSequence> sequences_;
    bool indexed_ = false;
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

} // namespace winalign
