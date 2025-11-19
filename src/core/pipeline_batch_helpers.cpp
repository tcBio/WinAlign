/**
 * @file pipeline_batch_helpers.cpp
 * @brief Batch processing helper functions implementation
 *
 * Provides utilities for building alignments from GPU results,
 * CPU-based alignment fallback, and batch processing.
 */

#include "pipeline_batch_helpers.h"
#include "winalign/reference_loader.h"
#include "winalign/bam_writer.h"
#include "winalign/sam_flags.h"
#include <string>

namespace winalign {
namespace internal {

BatchProcessingHelpers::BatchProcessingHelpers(
    ReferenceLoader* reference_loader,
    BamWriter* bam_writer,
    PipelineMetrics& metrics,
    double& mapping_quality_sum)
    : reference_loader_(reference_loader)
    , bam_writer_(bam_writer)
    , metrics_(metrics)
    , mapping_quality_sum_(mapping_quality_sum)
{
}

Alignment BatchProcessingHelpers::build_alignment_from_gpu_result(
    const cuda::AlignmentResult& result,
    const ReadView& view)
{
    Alignment alignment = build_unmapped_alignment(*view.read,
                                                   view.is_paired,
                                                   view.is_second);

    std::string contig;
    uint64_t local_offset = 0;
    if (!reference_loader_->map_global_position(result.position,
                                                contig,
                                                local_offset)) {
        return alignment;
    }

    alignment.reference_name = contig;
    alignment.position = local_offset;
    alignment.mapping_quality = result.mapping_quality;
    alignment.alignment_score = result.score;
    alignment.flag &= ~sam_flags::UNMAPPED;
    alignment.cigar = std::to_string(view.read->sequence.size()) + "M";
    return alignment;
}

Alignment BatchProcessingHelpers::build_unmapped_alignment(
    const Read& read,
    bool is_paired,
    bool is_second)
{
    Alignment alignment;
    alignment.read_id = read.id;
    alignment.reference_name = "*";
    alignment.position = 0;
    alignment.mapping_quality = 0;
    alignment.flag = 0;
    alignment.cigar = "*";
    alignment.alignment_score = 0;
    alignment.is_primary = true;

    if (is_paired) {
        alignment.flag |= sam_flags::PAIRED;
        alignment.flag |= is_second ? sam_flags::READ2 : sam_flags::READ1;
    }

    alignment.flag |= sam_flags::UNMAPPED;
    return alignment;
}

Alignment BatchProcessingHelpers::align_read_cpu(
    const Read& read,
    bool is_paired,
    bool is_second,
    bool& mapped)
{
    mapped = false;

    Alignment alignment = build_unmapped_alignment(read, is_paired, is_second);

    if (read.sequence.empty()) {
        return alignment;
    }

    // Simple exact match search across all reference sequences
    for (const auto& kv : reference_loader_->get_sequences()) {
        const auto& ref_name = kv.first;
        const auto& ref_seq = kv.second.sequence;
        auto pos = ref_seq.find(read.sequence);
        if (pos != std::string::npos) {
            alignment.reference_name = ref_name;
            alignment.position = static_cast<uint64_t>(pos);
            alignment.mapping_quality = 60;
            alignment.cigar = std::to_string(read.sequence.size()) + "M";
            mapped = true;
            alignment.flag &= ~sam_flags::UNMAPPED;
            break;
        }
    }

    if (!mapped) {
        alignment.flag |= sam_flags::UNMAPPED;
    }

    return alignment;
}

void BatchProcessingHelpers::process_cpu_alignment(
    const std::vector<ReadPair>& pairs,
    const std::vector<Read>& singles,
    bool is_paired)
{
    std::vector<Alignment> alignments;
    std::vector<Read> output_reads;

    if (is_paired) {
        for (const auto& pair : pairs) {
            bool mapped1 = false;
            bool mapped2 = false;
            Alignment aln1 = align_read_cpu(pair.read1, true, false, mapped1);
            Alignment aln2 = align_read_cpu(pair.read2, true, true, mapped2);

            alignments.push_back(aln1);
            output_reads.push_back(pair.read1);
            update_metrics(mapped1, aln1.mapping_quality);

            alignments.push_back(aln2);
            output_reads.push_back(pair.read2);
            update_metrics(mapped2, aln2.mapping_quality);

            if (mapped1 && mapped2 &&
                aln1.reference_name == aln2.reference_name) {
                metrics_.properly_paired++;
            }
        }
    } else {
        for (const auto& read : singles) {
            bool mapped = false;
            Alignment aln = align_read_cpu(read, false, false, mapped);
            alignments.push_back(aln);
            output_reads.push_back(read);
            update_metrics(mapped, aln.mapping_quality);
        }
    }

    if (!alignments.empty() && bam_writer_) {
        bam_writer_->write_batch(alignments, output_reads);
    }
}

void BatchProcessingHelpers::update_metrics(bool mapped, uint8_t mapq) {
    metrics_.total_reads++;
    if (mapped) {
        metrics_.aligned_reads++;
        mapping_quality_sum_ += mapq;
    }
    metrics_.mean_quality = metrics_.aligned_reads > 0
        ? mapping_quality_sum_ / static_cast<double>(metrics_.aligned_reads)
        : 0.0;
}

} // namespace internal
} // namespace winalign
