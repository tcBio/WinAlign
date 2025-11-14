#include "winalign/bam_writer.h"
#include <fstream>
#include <sstream>
#include <iostream>

namespace winalign {

class BamWriter::Impl {
public:
    Impl(const std::string& filename,
         const std::map<std::string, uint64_t>& reference_sequences)
        : filename_(filename), reference_sequences_(reference_sequences),
          compression_level_(6), threads_(1), create_index_(true),
          total_written_(0), is_open_(false) {}

    ~Impl() {
        close();
    }

    Result<bool> open() {
        if (is_open_) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR, "File already open");
        }

        // For Phase 3, we'll write simple SAM format (text)
        // Full BAM with htslib will be implemented in Phase 4
        file_.open(filename_, std::ios::out | std::ios::trunc);
        if (!file_.is_open()) {
            return Result<bool>(ErrorCode::FILE_NOT_FOUND,
                              "Failed to open file: " + filename_);
        }

        // Write SAM header
        write_header();

        is_open_ = true;
        std::cout << "SAM writer opened: " << filename_ << "\n";
        return Result<bool>(true);
    }

    void close() {
        if (is_open_ && file_.is_open()) {
            file_.close();
            is_open_ = false;
            std::cout << "SAM writer closed. Total alignments written: "
                     << total_written_ << "\n";
        }
    }

    bool is_open() const {
        return is_open_;
    }

    Result<bool> write(const Alignment& alignment, const Read& read) {
        if (!is_open_) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR, "File not open");
        }

        // Write SAM record
        write_sam_record(alignment, read);
        total_written_++;

        return Result<bool>(true);
    }

    size_t write_batch(const std::vector<Alignment>& alignments,
                      const std::vector<Read>& reads) {
        if (!is_open_) {
            return 0;
        }

        if (alignments.size() != reads.size()) {
            std::cerr << "Warning: alignments and reads size mismatch\n";
            return 0;
        }

        size_t written = 0;
        for (size_t i = 0; i < alignments.size(); ++i) {
            auto result = write(alignments[i], reads[i]);
            if (result.is_ok()) {
                written++;
            }
        }

        return written;
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
    void write_header() {
        // @HD - Header line
        file_ << "@HD\tVN:1.6\tSO:unsorted\n";

        // @SQ - Reference sequence dictionary
        for (const auto& [name, length] : reference_sequences_) {
            file_ << "@SQ\tSN:" << name << "\tLN:" << length << "\n";
        }

        // @PG - Program record
        file_ << "@PG\tID:WinAlign-GPU\tPN:WinAlign-GPU\tVN:" << VERSION
               << "\tCL:winalign-gpu\n";
    }

    void write_sam_record(const Alignment& alignment, const Read& read) {
        // SAM format (tab-delimited):
        // QNAME FLAG RNAME POS MAPQ CIGAR RNEXT PNEXT TLEN SEQ QUAL

        std::stringstream ss;

        // 1. QNAME - Read name
        ss << (read.name.empty() ? "read_" + std::to_string(read.id) : read.name);

        // 2. FLAG - SAM flags
        ss << "\t" << alignment.flag;

        // 3. RNAME - Reference sequence name
        ss << "\t" << (alignment.reference_name.empty() ? "*" : alignment.reference_name);

        // 4. POS - 1-based leftmost mapping position (SAM is 1-based, not 0-based)
        ss << "\t" << (alignment.position + 1);

        // 5. MAPQ - Mapping quality
        ss << "\t" << static_cast<int>(alignment.mapping_quality);

        // 6. CIGAR - CIGAR string
        ss << "\t" << (alignment.cigar.empty() ? "*" : alignment.cigar);

        // 7. RNEXT - Reference name of mate/next read
        ss << "\t*";

        // 8. PNEXT - Position of mate/next read
        ss << "\t0";

        // 9. TLEN - Template length (insert size for paired-end)
        ss << "\t0";

        // 10. SEQ - Read sequence
        ss << "\t" << (read.sequence.empty() ? "*" : read.sequence);

        // 11. QUAL - Quality string
        ss << "\t" << (read.quality.empty() ? "*" : read.quality);

        // Optional fields
        // AS:i - Alignment score
        ss << "\tAS:i:" << alignment.alignment_score;

        // Write line
        file_ << ss.str() << "\n";
    }

    std::string filename_;
    std::map<std::string, uint64_t> reference_sequences_;
    int compression_level_;
    int threads_;
    bool create_index_;
    uint64_t total_written_;
    bool is_open_;
    std::ofstream file_;
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
