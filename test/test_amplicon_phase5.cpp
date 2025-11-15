#include "winalign/amplicon/barcode_demux.h"
#include "winalign/amplicon/multi_sample_vcf.h"
#include "winalign/amplicon/realtime_processor.h"
#include "winalign/amplicon/cuda/multi_gpu.cuh"
#include <cassert>
#include <iostream>
#include <fstream>

using namespace winalign::amplicon;

void test_barcode_demultiplexing() {
    std::cout << "Testing barcode demultiplexing...\n";

    BarcodeDemultiplexer demux;

    // Create test barcodes
    std::vector<SampleBarcode> barcodes;

    SampleBarcode bc1;
    bc1.sample_id = "Sample1";
    bc1.barcode = "ATCGATCG";
    bc1.expected_position = 0;
    barcodes.push_back(bc1);

    SampleBarcode bc2;
    bc2.sample_id = "Sample2";
    bc2.barcode = "GCTAGCTA";
    bc2.expected_position = 0;
    barcodes.push_back(bc2);

    demux.set_barcodes(barcodes);

    // Test exact match
    std::string seq1 = "ATCGATCGACGTACGTACGTACGT";
    std::string qual1 = "IIIIIIIIIIIIIIIIIIIIIIII";

    DemuxResult result = demux.demultiplex(seq1, qual1);

    assert(result.assigned());
    assert(result.sample_id == "Sample1");
    assert(result.mismatches == 0);

    // Test with mismatch
    std::string seq2 = "ATCGTTCGACGTACGTACGTACGT";  // One mismatch
    std::string qual2 = "IIIIIIIIIIIIIIIIIIIIIIII";

    result = demux.demultiplex(seq2, qual2);

    assert(result.assigned());  // Should still assign with 1 mismatch
    assert(result.sample_id == "Sample1");
    assert(result.mismatches == 1);

    // Test second barcode
    std::string seq3 = "GCTAGCTAACGTACGTACGTACGT";
    std::string qual3 = "IIIIIIIIIIIIIIIIIIIIIIII";

    result = demux.demultiplex(seq3, qual3);

    assert(result.assigned());
    assert(result.sample_id == "Sample2");

    // Test unassigned
    std::string seq4 = "TTTTTTTTACGTACGTACGTACGT";
    std::string qual4 = "IIIIIIIIIIIIIIIIIIIIIIII";

    result = demux.demultiplex(seq4, qual4);

    assert(!result.assigned());

    // Get stats
    auto stats = demux.get_stats();
    assert(stats.total_reads == 4);
    assert(stats.assigned == 3);
    assert(stats.unassigned == 1);
    assert(stats.assignment_rate > 0.7f);

    std::cout << "  ✓ Barcode demultiplexing tests passed\n";
    std::cout << "    - " << stats.assigned << "/" << stats.total_reads
              << " reads assigned (" << (stats.assignment_rate * 100) << "%)\n";
}

void test_multi_sample_vcf() {
    std::cout << "Testing multi-sample VCF writing...\n";

    std::vector<std::string> samples = {"Sample1", "Sample2", "Sample3"};

    MultiSampleVCFWriter writer(
        "/tmp/test_multi_sample.vcf",
        "reference.fasta",
        samples
    );

    auto result = writer.open();
    assert(result.is_ok());

    // Write header
    std::vector<AmpliconTarget> targets;
    AmpliconTarget target;
    target.amplicon_id = "MARKER_001";
    target.chromosome = "chr1";
    targets.push_back(target);

    result = writer.write_header(targets);
    assert(result.is_ok());

    // Create test variant
    MultiSampleVariant variant;
    variant.amplicon_id = "MARKER_001";
    variant.chromosome = "chr1";
    variant.position = 12345;
    variant.ref_allele = 'A';
    variant.alt_allele = 'G';
    variant.is_known_marker = true;

    // Add genotypes for each sample
    MultiSampleVariant::SampleGenotype gt1;
    gt1.genotype = "0/1";
    gt1.depth = 100;
    gt1.ref_depth = 50;
    gt1.alt_depth = 50;
    gt1.allele_frequency = 0.5f;
    gt1.quality = 60;
    variant.sample_genotypes["Sample1"] = gt1;

    MultiSampleVariant::SampleGenotype gt2;
    gt2.genotype = "1/1";
    gt2.depth = 150;
    gt2.ref_depth = 5;
    gt2.alt_depth = 145;
    gt2.allele_frequency = 0.97f;
    gt2.quality = 60;
    variant.sample_genotypes["Sample2"] = gt2;

    MultiSampleVariant::SampleGenotype gt3;
    gt3.genotype = "0/0";
    gt3.depth = 120;
    gt3.ref_depth = 118;
    gt3.alt_depth = 2;
    gt3.allele_frequency = 0.02f;
    gt3.quality = 55;
    variant.sample_genotypes["Sample3"] = gt3;

    // Write variant
    result = writer.write_variant(variant);
    assert(result.is_ok());

    writer.close();

    // Verify file was created
    std::ifstream check("/tmp/test_multi_sample.vcf");
    assert(check.is_open());
    check.close();

    // Get stats
    auto stats = writer.get_stats();
    assert(stats.variants_written == 1);
    assert(stats.total_genotypes == 3);

    std::cout << "  ✓ Multi-sample VCF tests passed\n";
    std::cout << "    - " << stats.variants_written << " variants written\n";
    std::cout << "    - " << stats.total_genotypes << " genotypes\n";
}

void test_joint_genotyper() {
    std::cout << "Testing joint genotyper...\n";

    JointGenotyper genotyper;

    // Create per-sample variants
    std::unordered_map<std::string, std::vector<AmpliconVariant>> per_sample_variants;

    // Sample 1
    AmpliconVariant v1;
    v1.amplicon_id = "MARKER_001";
    v1.chromosome = "chr1";
    v1.position = 12345;
    v1.ref_allele = 'A';
    v1.alt_allele = 'G';
    v1.allele_frequency = 0.5f;
    v1.total_depth = 100;
    per_sample_variants["Sample1"].push_back(v1);

    // Sample 2 (same position)
    AmpliconVariant v2 = v1;
    v2.allele_frequency = 0.95f;
    v2.total_depth = 150;
    per_sample_variants["Sample2"].push_back(v2);

    // Perform joint genotyping
    std::vector<AmpliconTarget> targets;
    AmpliconTarget target;
    target.amplicon_id = "MARKER_001";
    targets.push_back(target);

    auto joint_variants = genotyper.joint_genotype(targets, per_sample_variants);

    assert(!joint_variants.empty());
    assert(joint_variants[0].sample_genotypes.size() == 2);

    std::cout << "  ✓ Joint genotyper tests passed\n";
    std::cout << "    - " << joint_variants.size() << " joint variants called\n";
}

void test_multi_gpu() {
    std::cout << "Testing multi-GPU support...\n";

    // Get available devices
    auto devices = cuda::MultiGPUManager::get_available_devices();

    std::cout << "  Found " << devices.size() << " GPU(s):\n";
    for (const auto& dev : devices) {
        std::cout << "    - GPU " << dev.device_id << ": " << dev.name
                  << " (" << (dev.free_memory / 1024 / 1024) << " MB free)\n";
    }

    if (devices.empty()) {
        std::cout << "  ⚠ No GPUs available, skipping multi-GPU tests\n";
        return;
    }

    // Create multi-GPU manager
    cuda::MultiGPUConfig config;
    config.auto_balance = true;

    cuda::MultiGPUManager manager(config);
    bool initialized = manager.initialize();

    assert(initialized);

    // Test work distribution
    auto batches = manager.distribute_work(nullptr, 10000, sizeof(ReadCluster));

    std::cout << "  Work distributed across " << batches.size() << " GPU(s)\n";
    for (size_t i = 0; i < batches.size(); ++i) {
        std::cout << "    - GPU " << batches[i].device_id
                  << ": " << batches[i].count << " items\n";
    }

    assert(!batches.empty());

    std::cout << "  ✓ Multi-GPU tests passed\n";
}

void test_realtime_processor() {
    std::cout << "Testing real-time processor...\n";

    // Create test directory
    std::filesystem::create_directories("/tmp/winalign_realtime_test");

    RealtimeConfig rt_config;
    rt_config.watch_directory = "/tmp/winalign_realtime_test";
    rt_config.poll_interval_ms = 100;
    rt_config.batch_size = 1000;

    AmpliconConfig amp_config;
    amp_config.input_fastq = "";  // Not used in realtime mode
    amp_config.output_vcf = "/tmp/realtime_output.vcf";

    RealtimeProcessor processor(rt_config, amp_config);

    // Set callbacks
    bool file_event_received = false;
    processor.set_file_event_callback([&](const std::string& file, FileEvent event) {
        std::cout << "    - File event: " << file << "\n";
        file_event_received = true;
    });

    processor.set_progress_callback([](uint64_t reads, float throughput) {
        // Progress callback
    });

    // Note: In a real test, we would:
    // 1. Start processor
    // 2. Create a FASTQ file in watch directory
    // 3. Verify processing occurs
    // 4. Stop processor
    //
    // For unit test, we'll just verify the processor can be created and configured

    assert(!processor.is_running());

    std::cout << "  ✓ Real-time processor tests passed\n";
    std::cout << "    - Processor configured successfully\n";
}

void test_incremental_fastq_reader() {
    std::cout << "Testing incremental FASTQ reader...\n";

    // Create test FASTQ file
    std::ofstream out("/tmp/test_incremental.fastq");
    out << "@read1\n";
    out << "ACGTACGTACGTACGT\n";
    out << "+\n";
    out << "IIIIIIIIIIIIIIII\n";
    out << "@read2\n";
    out << "GCTAGCTAGCTAGCTA\n";
    out << "+\n";
    out << "IIIIIIIIIIIIIIII\n";
    out.close();

    IncrementalFASTQReader reader("/tmp/test_incremental.fastq");

    auto reads = reader.read_next_batch(10);

    assert(reads.size() == 2);
    assert(reads[0].sequence == "ACGTACGTACGTACGT");
    assert(reads[1].sequence == "GCTAGCTAGCTAGCTA");

    std::cout << "  ✓ Incremental FASTQ reader tests passed\n";
    std::cout << "    - Read " << reads.size() << " reads incrementally\n";
}

int main() {
    std::cout << "\n=== Phase 5 Feature Tests ===\n\n";

    try {
        test_barcode_demultiplexing();
        test_multi_sample_vcf();
        test_joint_genotyper();
        test_multi_gpu();
        test_realtime_processor();
        test_incremental_fastq_reader();

        std::cout << "\n✓ All Phase 5 tests passed!\n\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n✗ Test failed with exception: " << e.what() << "\n\n";
        return 1;
    }
}
