/**
 * @file pipeline_batch_helpers.h
 * @brief Helper functions for batch processing and alignment building
 */

#pragma once

#include "pipeline_internal.h"
#include "winalign/common.h"
#include "winalign/cuda/alignment.cuh"
#include <cstdint>
#include <string>

namespace winalign {

// Forward declarations
class ReferenceLoader;
class BamWriter;
struct PipelineMetrics;

namespace internal {

/**
 * @brief Helper functions for building alignments and processing batches
 *
 * Contains utility functions for converting GPU results to alignments,
 * CPU-based alignment fallback, and batch processing helpers.
 */
class BatchProcessingHelpers {
public:
    /**
     * @brief Construct batch processing helpers
     * @param reference_loader Reference loader for mapping positions
     * @param bam_writer BAM writer for output
     * @param metrics Pipeline metrics (for updating)
     * @param mapping_quality_sum Cumulative mapping quality sum
     */
    BatchProcessingHelpers(
        ReferenceLoader* reference_loader,
        BamWriter* bam_writer,
        PipelineMetrics& metrics,
        double& mapping_quality_sum);

    /**
     * @brief Build alignment from GPU alignment result
     * @param result GPU alignment result
     * @param view Read view with metadata
     * @return Populated Alignment structure
     */
    Alignment build_alignment_from_gpu_result(
        const cuda::AlignmentResult& result,
        const ReadView& view);

    /**
     * @brief Build unmapped alignment for a read
     * @param read The read
     * @param is_paired Whether read is from paired-end sequencing
     * @param is_second Whether this is the second read in pair
     * @return Unmapped alignment with appropriate flags
     */
    Alignment build_unmapped_alignment(
        const Read& read,
        bool is_paired,
        bool is_second);

    /**
     * @brief Align read using CPU (simple exact match search)
     * @param read The read to align
     * @param is_paired Whether read is from paired-end sequencing
     * @param is_second Whether this is the second read in pair
     * @param mapped Output parameter set to true if aligned
     * @return Alignment (mapped or unmapped)
     */
    Alignment align_read_cpu(
        const Read& read,
        bool is_paired,
        bool is_second,
        bool& mapped);

    /**
     * @brief Process batch of reads using CPU alignment
     * @param pairs Paired-end reads (if is_paired=true)
     * @param singles Single-end reads (if is_paired=false)
     * @param is_paired Whether data is paired-end
     */
    void process_cpu_alignment(
        const std::vector<ReadPair>& pairs,
        const std::vector<Read>& singles,
        bool is_paired);

    /**
     * @brief Update pipeline metrics with alignment result
     * @param mapped Whether read was successfully mapped
     * @param mapq Mapping quality score
     */
    void update_metrics(bool mapped, uint8_t mapq);

private:
    ReferenceLoader* reference_loader_;
    BamWriter* bam_writer_;
    PipelineMetrics& metrics_;
    double& mapping_quality_sum_;
};

} // namespace internal
} // namespace winalign
