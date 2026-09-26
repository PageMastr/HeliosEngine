// GP-1 (corpus clauses): the shared C++/Go HXL corpus (tests/corpus/hxl) passes bit for bit,
// including >= 1,000 FMA-sensitive vectors (06 §1.2 rule 7). Go runs the same files in
// services/pkg/hxl (corpus_test.go).
#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>
#include <string>

#include "corpus.h"

using namespace helios;
using namespace helios::hxl::test;

TEST_CASE("hxl corpus: every case matches bit for bit") {
    const CorpusStats stats = runCorpus(HELIOS_HXL_CORPUS_DIR);
    for (const std::string& f : stats.failures) FAIL_CHECK(f);
    CHECK(stats.failures.empty());
    MESSAGE("hxl corpus: " << stats.files << " files, " << stats.cases << " cases (" << stats.errorCases
                           << " compile errors), " << stats.evaluations << " evaluations, " << stats.fmaRows
                           << " FMA-sensitive rows, " << stats.bytecodeHashes << " bytecode hashes");
    CHECK(stats.files >= 5);
    CHECK(stats.cases >= 150);
    CHECK(stats.errorCases >= 30);
    CHECK(stats.fmaRows >= 1000);
    CHECK(stats.bytecodeHashes >= 100);
}

TEST_CASE("hxl corpus: number syntax") {
    f64 v = 0;
    CHECK(parseCorpusNumber("0x1.8p+1", v));
    CHECK(v == 3.0);
    CHECK(parseCorpusNumber("-0x1p-1074", v));
    CHECK(std::bit_cast<u64>(v) == 0x8000000000000001ull);
    CHECK(parseCorpusNumber("0x0.0000000000001p-1022", v));
    CHECK(std::bit_cast<u64>(v) == 1ull);
    CHECK(parseCorpusNumber("0x1.fffffffffffffp+1023", v));
    CHECK(v == std::numeric_limits<f64>::max());
    CHECK_FALSE(parseCorpusNumber("0x1p+1024", v));           // overflow
    CHECK_FALSE(parseCorpusNumber("0x1.00000000000001p+0", v)); // 56 bits: inexact
    CHECK_FALSE(parseCorpusNumber("0x1p-1075", v));           // below the smallest subnormal
    CHECK_FALSE(parseCorpusNumber("0x1.8", v));               // exponent required
    CHECK(parseCorpusNumber("-0", v));
    CHECK(std::bit_cast<u64>(v) == 0x8000000000000000ull);
    CHECK(parseCorpusNumber("0.1", v));
    CHECK(v == 0.1);
    CHECK(parseCorpusNumber("-inf", v));
    CHECK(v == -std::numeric_limits<f64>::infinity());
    CHECK(parseCorpusNumber("nan", v));
    CHECK(std::isnan(v));
    CHECK_FALSE(parseCorpusNumber("1.5x", v));
    CHECK_FALSE(parseCorpusNumber("", v));
}
