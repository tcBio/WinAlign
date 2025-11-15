#include "winalign/amplicon/read_collapser.h"
#include "winalign/amplicon/amplicon_assigner.h"
#include "winalign/amplicon/config_parser.h"
#include "winalign/amplicon/amplicon_types.h"
#include <iostream>
#include <cassert>

using namespace winalign;
using namespace winalign::amplicon;

void test_read_collapser() {
    std::cout << "Testing ReadCollapser...\n";

    ReadCollapser collapser(2);

    // Create test reads (10 identical reads)
    std::vector<Read> reads;
    for (int i = 0; i < 10; ++i) {
        Read read;
        read.id = i;
        read.name = "read_" + std::to_string(i);
        read.sequence = "ACGTACGTACGTACGT";
        read.quality = "IIIIIIIIIIIIIIII";
        reads.push_back(read);
    }

    // Add some slightly different reads
    for (int i = 10; i < 15; ++i) {
        Read read;
        read.id = i;
        read.name = "read_" + std::to_string(i);
        read.sequence = "ACGTACGTACGTACGG"; // 1bp difference
        read.quality = "IIIIIIIIIIIIIIII";
        reads.push_back(read);
    }

    auto clusters = collapser.collapse(reads);
    auto stats = collapser.get_stats();

    std::cout << "  Input reads: " << stats.input_reads << "\n";
    std::cout << "  Output clusters: " << stats.output_clusters << "\n";
    std::cout << "  Compression ratio: " << stats.compression_ratio << "x\n";
    std::cout << "  Avg cluster size: " << stats.avg_cluster_size << "\n";

    assert(stats.input_reads == 15);
    assert(stats.output_clusters <= 2); // Should collapse to 1-2 clusters
    assert(stats.compression_ratio > 5.0f);

    std::cout << "  ✓ ReadCollapser test passed!\n\n";
}

void test_amplicon_assigner() {
    std::cout << "Testing AmpliconAssigner...\n";

    // Create test amplicon targets
    std::vector<AmpliconTarget> targets;

    AmpliconTarget target1;
    target1.amplicon_id = "AMP_001";
    target1.chromosome = "chr1";
    target1.start = 1000;
    target1.end = 1200;
    target1.primer_fwd = "ACGTACGT";
    target1.primer_rev = "GCTAGCTA";
    target1.expected_length = 200;
    targets.push_back(target1);

    AmpliconTarget target2;
    target2.amplicon_id = "AMP_002";
    target2.chromosome = "chr2";
    target2.start = 2000;
    target2.end = 2200;
    target2.primer_fwd = "TGCATGCA";
    target2.primer_rev = "ATCGATCG";
    target2.expected_length = 200;
    targets.push_back(target2);

    AmpliconAssigner assigner(targets);

    // Create test clusters
    std::vector<ReadCluster> clusters;

    // Cluster with primer_fwd from target1
    ReadCluster cluster1;
    cluster1.consensus_sequence = "ACGTACGTNNNNNNNNNNNNNNNN"; // Starts with primer
    cluster1.read_count = 100;
    clusters.push_back(cluster1);

    // Cluster with primer_fwd from target2
    ReadCluster cluster2;
    cluster2.consensus_sequence = "TGCATGCANNNNNNNNNNNNNNN";
    cluster2.read_count = 50;
    clusters.push_back(cluster2);

    // Cluster with no matching primer
    ReadCluster cluster3;
    cluster3.consensus_sequence = "NNNNNNNNNNNNNNNNNNNN";
    cluster3.read_count = 10;
    clusters.push_back(cluster3);

    // Assign
    assigner.assign_batch(clusters);
    auto stats = assigner.get_stats();

    std::cout << "  Total clusters: " << stats.total_clusters << "\n";
    std::cout << "  Assigned: " << stats.assigned_clusters << "\n";
    std::cout << "  Unassigned: " << stats.unassigned_clusters << "\n";

    for (const auto& [amp_id, count] : stats.per_amplicon_counts) {
        std::cout << "    " << amp_id << ": " << count << " clusters\n";
    }

    assert(stats.total_clusters == 3);
    assert(stats.assigned_clusters >= 2);
    assert(clusters[0].amplicon_id == "AMP_001");
    assert(clusters[1].amplicon_id == "AMP_002");

    std::cout << "  ✓ AmpliconAssigner test passed!\n\n";
}

void test_config_parser() {
    std::cout << "Testing ConfigParser...\n";

    std::string yaml_config = R"(
amplicon_panel:
  name: "Test Panel"
  reference: "test_ref.fasta"

targets:
  - amplicon_id: "TEST_001"
    chromosome: "chr1"
    start: 1000
    end: 1200
    primer_fwd: "ACGTACGT"
    primer_rev: "GCTAGCTA"
    snp_positions: [1050, 1100]

  - amplicon_id: "TEST_002"
    chromosome: "chr2"
    start: 2000
    end: 2200
    primer_fwd: "TGCATGCA"
    primer_rev: "ATCGATCG"
    snp_positions: [2100]

processing:
  collapse_reads: true
  max_cluster_distance: 2
  min_depth: 100
  min_allele_frequency: 0.01
  quality_threshold: 20
)";

    ConfigParser parser;
    auto result = parser.load_from_string(yaml_config);

    assert(result.is_ok());

    const auto& config = result.value;

    std::cout << "  Panel name: " << config.panel_name << "\n";
    std::cout << "  Reference: " << config.reference_fasta << "\n";
    std::cout << "  Targets: " << config.targets.size() << "\n";
    std::cout << "  Collapse reads: " << (config.collapse_reads ? "yes" : "no") << "\n";
    std::cout << "  Max cluster distance: " << config.max_cluster_distance << "\n";
    std::cout << "  Min depth: " << config.min_depth << "\n";

    assert(config.panel_name == "Test Panel");
    assert(config.reference_fasta == "test_ref.fasta");
    assert(config.targets.size() == 2);
    assert(config.targets[0].amplicon_id == "TEST_001");
    assert(config.targets[0].snp_positions.size() == 2);
    assert(config.targets[1].snp_positions.size() == 1);
    assert(config.collapse_reads == true);
    assert(config.max_cluster_distance == 2);
    assert(config.min_depth == 100);

    std::cout << "  ✓ ConfigParser test passed!\n\n";
}

int main() {
    std::cout << "=== WinAlign-Amplicon Component Tests ===\n\n";

    try {
        test_read_collapser();
        test_amplicon_assigner();
        test_config_parser();

        std::cout << "=== All tests passed! ===\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
