#include "winalign/fastq_parser.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <zlib.h>

namespace winalign {

// FastqParser implementation
class FastqParser::Impl {
public:
    Impl(const std::string& filename, bool is_gzipped)
        : filename_(filename), is_gzipped_(is_gzipped), total_reads_(0),
          gzfile_(nullptr), file_(), eof_(false) {
        // Auto-detect gzip if not specified
        if (!is_gzipped_) {
            is_gzipped_ = (filename.length() > 3 &&
                          filename.substr(filename.length() - 3) == ".gz");
        }
    }

    ~Impl() {
        close();
    }

    Result<bool> open() {
        if (is_gzipped_) {
            gzfile_ = gzopen(filename_.c_str(), "rb");
            if (!gzfile_) {
                return Result<bool>(ErrorCode::FILE_NOT_FOUND,
                                  "Failed to open gzipped file: " + filename_);
            }
            // Set buffer size for better performance
            gzbuffer(gzfile_, 128 * 1024); // 128KB buffer
        } else {
            file_.open(filename_, std::ios::binary);
            if (!file_.is_open()) {
                return Result<bool>(ErrorCode::FILE_NOT_FOUND,
                                  "Failed to open file: " + filename_);
            }
        }

        eof_ = false;
        return Result<bool>(true);
    }

    void close() {
        if (is_gzipped_ && gzfile_) {
            gzclose(gzfile_);
            gzfile_ = nullptr;
        } else if (file_.is_open()) {
            file_.close();
        }
        eof_ = true;
    }

    bool is_open() const {
        if (is_gzipped_) {
            return gzfile_ != nullptr;
        } else {
            return file_.is_open();
        }
    }

    bool next(Read& read) {
        if (eof_) return false;

        // FASTQ format:
        // @read_name
        // sequence
        // +
        // quality

        std::string line;

        // Line 1: Read name (starts with @)
        if (!read_line(line) || line.empty() || line[0] != '@') {
            eof_ = true;
            return false;
        }
        read.name = line.substr(1); // Remove '@'
        read.id = total_reads_;

        // Line 2: Sequence
        if (!read_line(line)) {
            eof_ = true;
            return false;
        }
        read.sequence = line;

        // Line 3: Separator (starts with +)
        if (!read_line(line) || line.empty() || line[0] != '+') {
            eof_ = true;
            return false;
        }

        // Line 4: Quality scores
        if (!read_line(line)) {
            eof_ = true;
            return false;
        }
        read.quality = line;

        // Validate lengths match
        if (read.sequence.length() != read.quality.length()) {
            eof_ = true;
            return false;
        }

        total_reads_++;
        return true;
    }

    size_t next_batch(std::vector<Read>& reads, size_t batch_size) {
        reads.clear();
        reads.reserve(batch_size);

        for (size_t i = 0; i < batch_size; ++i) {
            Read read;
            if (!next(read)) break;
            reads.push_back(std::move(read));
        }

        return reads.size();
    }

    uint64_t total_reads() const {
        return total_reads_;
    }

    bool eof() const {
        return eof_;
    }

private:
    // Read a line from file (gzipped or not)
    bool read_line(std::string& line) {
        line.clear();

        if (is_gzipped_) {
            char buffer[4096];
            if (gzgets(gzfile_, buffer, sizeof(buffer)) == nullptr) {
                return false;
            }
            line = buffer;
        } else {
            if (!std::getline(file_, line)) {
                return false;
            }
        }

        // Remove trailing newline/carriage return
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }

        return true;
    }

    std::string filename_;
    bool is_gzipped_;
    uint64_t total_reads_;
    gzFile gzfile_;           // For gzipped files
    std::ifstream file_;      // For uncompressed files
    bool eof_;
};

FastqParser::FastqParser(const std::string& filename, bool is_gzipped)
    : pimpl_(std::make_unique<Impl>(filename, is_gzipped)) {}

FastqParser::~FastqParser() = default;

FastqParser::FastqParser(FastqParser&&) noexcept = default;
FastqParser& FastqParser::operator=(FastqParser&&) noexcept = default;

Result<bool> FastqParser::open() { return pimpl_->open(); }
void FastqParser::close() { pimpl_->close(); }
bool FastqParser::is_open() const { return pimpl_->is_open(); }
bool FastqParser::next(Read& read) { return pimpl_->next(read); }
size_t FastqParser::next_batch(std::vector<Read>& reads, size_t batch_size) {
    return pimpl_->next_batch(reads, batch_size);
}
uint64_t FastqParser::total_reads() const { return pimpl_->total_reads(); }
bool FastqParser::eof() const { return pimpl_->eof(); }

// PairedFastqParser implementation
class PairedFastqParser::Impl {
public:
    Impl(const std::string& filename1, const std::string& filename2)
        : parser1_(filename1), parser2_(filename2) {}

    Result<bool> open() {
        auto r1 = parser1_.open();
        if (!r1.is_ok()) return r1;
        auto r2 = parser2_.open();
        if (!r2.is_ok()) return r2;
        return Result<bool>(true);
    }

    void close() {
        parser1_.close();
        parser2_.close();
    }

    bool next(ReadPair& pair) {
        return parser1_.next(pair.read1) && parser2_.next(pair.read2);
    }

    size_t next_batch(std::vector<ReadPair>& pairs, size_t batch_size) {
        // TODO: Optimize batch reading
        pairs.clear();
        pairs.reserve(batch_size);
        for (size_t i = 0; i < batch_size; ++i) {
            ReadPair pair;
            if (!next(pair)) break;
            pairs.push_back(std::move(pair));
        }
        return pairs.size();
    }

    uint64_t total_pairs() const {
        return parser1_.total_reads();
    }

    bool eof() const {
        return parser1_.eof() && parser2_.eof();
    }

private:
    FastqParser parser1_;
    FastqParser parser2_;
};

PairedFastqParser::PairedFastqParser(const std::string& filename1,
                                     const std::string& filename2)
    : pimpl_(std::make_unique<Impl>(filename1, filename2)) {}

PairedFastqParser::~PairedFastqParser() = default;

Result<bool> PairedFastqParser::open() { return pimpl_->open(); }
void PairedFastqParser::close() { pimpl_->close(); }
bool PairedFastqParser::next(ReadPair& pair) { return pimpl_->next(pair); }
size_t PairedFastqParser::next_batch(std::vector<ReadPair>& pairs, size_t batch_size) {
    return pimpl_->next_batch(pairs, batch_size);
}
uint64_t PairedFastqParser::total_pairs() const { return pimpl_->total_pairs(); }
bool PairedFastqParser::eof() const { return pimpl_->eof(); }

} // namespace winalign
