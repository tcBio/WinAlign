#include "winalign/bam_writer.h"

namespace winalign {

class BamWriter::Impl {
public:
    Impl(const std::string& filename,
         const std::map<std::string, uint64_t>& reference_sequences)
        : filename_(filename), reference_sequences_(reference_sequences) {}

    Result<bool> open() {
        // TODO: Open BAM file with htslib
        return Result<bool>(ErrorCode::RUNTIME_ERROR, "Not implemented");
    }

    void close() {
        // TODO: Close BAM file
    }

    bool is_open() const {
        return false; // TODO
    }

    Result<bool> write(const Alignment& alignment, const Read& read) {
        // TODO: Write single alignment
        return Result<bool>(ErrorCode::RUNTIME_ERROR, "Not implemented");
    }

    size_t write_batch(const std::vector<Alignment>& alignments,
                      const std::vector<Read>& reads) {
        // TODO: Write batch of alignments
        return 0;
    }

    void set_compression_level(int level) {
        compression_level_ = level;
    }

    void set_threads(int threads) {
        threads_ = threads;
    }

    void enable_index(bool enable) {
        create_index_ = enable;
    }

    uint64_t total_written() const {
        return total_written_;
    }

private:
    std::string filename_;
    std::map<std::string, uint64_t> reference_sequences_;
    int compression_level_ = 6;
    int threads_ = 1;
    bool create_index_ = true;
    uint64_t total_written_ = 0;
};

BamWriter::BamWriter(const std::string& filename,
                     const std::map<std::string, uint64_t>& reference_sequences)
    : pimpl_(std::make_unique<Impl>(filename, reference_sequences)) {}

BamWriter::~BamWriter() = default;

Result<bool> BamWriter::open() { return pimpl_->open(); }
void BamWriter::close() { pimpl_->close(); }
bool BamWriter::is_open() const { return pimpl_->is_open(); }
Result<bool> BamWriter::write(const Alignment& alignment, const Read& read) {
    return pimpl_->write(alignment, read);
}
size_t BamWriter::write_batch(const std::vector<Alignment>& alignments,
                              const std::vector<Read>& reads) {
    return pimpl_->write_batch(alignments, reads);
}
void BamWriter::set_compression_level(int level) {
    pimpl_->set_compression_level(level);
}
void BamWriter::set_threads(int threads) {
    pimpl_->set_threads(threads);
}
void BamWriter::enable_index(bool enable) {
    pimpl_->enable_index(enable);
}
uint64_t BamWriter::total_written() const {
    return pimpl_->total_written();
}

} // namespace winalign
