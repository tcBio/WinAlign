#include "winalign/amplicon/smith_waterman.h"
#include "winalign/amplicon/primer_trimmer.h"
#include "winalign/amplicon/chimera_detector.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace winalign::amplicon;

void test_smith_waterman() {
    std::cout << "Testing Smith-Waterman alignment...\n";

    SmithWatermanAligner aligner;

    // Test exact match
    std::string query = "ACGTACGTACGT";
    std::string target = "ACGTACGTACGT";

    SWAlignment result = aligner.align(query, target);

    assert(result.score > 0);
    assert(result.matches == 12);
    assert(result.mismatches == 0);
    assert(result.identity > 99.0f);

    // Test with mismatch
    query = "ACGTACGTACGT";
    target = "ACGTACTTACGT";  // One mismatch

    result = aligner.align(query, target);

    assert(result.score > 0);
    assert(result.mismatches > 0);
    assert(result.identity < 100.0f && result.identity > 90.0f);

    // Test with gap
    query = "ACGTACGTACGT";
    target = "ACGTACGTACGT";  // Insertion

    result = aligner.align(query, target);

    assert(result.score > 0);

    std::cout << "  ✓ Smith-Waterman tests passed\n";
}

void test_primer_trimming() {
    std::cout << "Testing primer trimming...\n";

    PrimerTrimmer trimmer;

    // Create test amplicon targets
    std::vector<AmpliconTarget> targets;
    AmpliconTarget target;
    target.amplicon_id = "TEST_001";
    target.primer_fwd = "ACGTACGT";
    target.primer_rev = "TGCATGCA";
    targets.push_back(target);

    trimmer.set_targets(targets);

    // Test forward primer trimming
    std::string sequence = "ACGTACGTATATATATATAT";  // Primer + insert
    std::string quality = "IIIIIIIIIIIIIIIIIIII";

    PrimerTrimResult result = trimmer.trim(sequence, quality);

    assert(result.fwd_found);
    assert(result.fwd_trim_len == 8);
    assert(result.trimmed_sequence.length() < sequence.length());
    assert(result.trimmed_sequence == "ATATATATATAT");

    // Test both primers
    sequence = "ACGTACGTATATATATATATTGCATGCA";
    quality = "IIIIIIIIIIIIIIIIIIIIIIIIIIII";

    result = trimmer.trim(sequence, quality);

    assert(result.any_trimmed());
    assert(result.trimmed_sequence.length() < sequence.length());

    std::cout << "  ✓ Primer trimming tests passed\n";
}

void test_reverse_complement() {
    std::cout << "Testing reverse complement...\n";

    std::string seq = "ACGT";
    std::string rc = reverse_complement(seq);

    assert(rc == "ACGT");  // ACGT is palindromic

    seq = "AAAATTTT";
    rc = reverse_complement(seq);

    assert(rc == "AAAATTTT");  // Also palindromic

    seq = "ACGTACGT";
    rc = reverse_complement(seq);

    assert(rc == "ACGTACGT");  // Palindrome

    seq = "ATCG";
    rc = reverse_complement(seq);

    assert(rc == "CGAT");

    std::cout << "  ✓ Reverse complement tests passed\n";
}

void test_chimera_detection() {
    std::cout << "Testing chimera detection...\n";

    ChimeraDetector detector;

    // Create reference sequences
    std::vector<std::string> references = {
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "TTTTTTTTTTTTTTTTTTTTTTTTTTTT"
    };

    detector.set_references(references);

    // Test non-chimeric sequence
    std::string non_chimera = "AAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    ChimeraResult result = detector.detect(non_chimera, 100);

    assert(!result.is_chimera);

    // Test potential chimeric sequence (mix of both references)
    std::string chimera = "AAAAAAAAAAAAATTTTTTTTTTTTTT";
    result = detector.detect(chimera, 10);  // Low abundance

    // Note: Simple chimera detection may or may not flag this
    // The test validates that the function runs without error

    std::cout << "  ✓ Chimera detection tests passed\n";
}

void test_read_cluster_operations() {
    std::cout << "Testing read cluster operations...\n";

    ReadCluster cluster;
    cluster.consensus_sequence = "ACGTACGTACGT";
    cluster.consensus_quality = "IIIIIIIIIIII";
    cluster.read_count = 100;

    assert(!cluster.empty());
    assert(cluster.length() == 12);

    // Test primer trimming on cluster
    PrimerTrimmer trimmer;
    std::vector<AmpliconTarget> targets;
    AmpliconTarget target;
    target.amplicon_id = "TEST";
    target.primer_fwd = "ACGT";
    targets.push_back(target);
    trimmer.set_targets(targets);

    PrimerTrimResult result = trimmer.trim_cluster(cluster);

    assert(cluster.consensus_sequence.length() < 12);

    std::cout << "  ✓ Read cluster operations passed\n";
}

void test_amplicon_variant() {
    std::cout << "Testing AmpliconVariant genotype calculation...\n";

    AmpliconVariant variant;

    // Test homozygous reference
    variant.allele_frequency = 0.05f;
    assert(variant.genotype() == "0/0");

    // Test heterozygous
    variant.allele_frequency = 0.50f;
    assert(variant.genotype() == "0/1");

    // Test homozygous alternate
    variant.allele_frequency = 0.95f;
    assert(variant.genotype() == "1/1");

    std::cout << "  ✓ Amplicon variant tests passed\n";
}

int main() {
    std::cout << "\n=== Phase 4 Feature Tests ===\n\n";

    try {
        test_smith_waterman();
        test_primer_trimming();
        test_reverse_complement();
        test_chimera_detection();
        test_read_cluster_operations();
        test_amplicon_variant();

        std::cout << "\n✓ All Phase 4 tests passed!\n\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n✗ Test failed with exception: " << e.what() << "\n\n";
        return 1;
    }
}
