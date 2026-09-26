#pragma once
// Runner for the shared HXL conformance corpus (tests/corpus/hxl/*.jsonc; format in its README).
// The Go twin is services/pkg/hxl/corpus_test.go; both must accept exactly the same files.

#include <string>
#include <string_view>
#include <vector>

#include "helios/core/types.h"

namespace helios::hxl::test {

/// Parses a corpus number: "nan", "inf", "-inf", exact hex floats ("-0x1.8p+1", "0x1p-1074") or
/// decimal text (correctly rounded). Hex floats must be exactly representable.
bool parseCorpusNumber(std::string_view text, f64& out);

/// "0x3ff8000000000000 (1.5)".
std::string describeBits(f64 value);

struct CorpusStats {
    usize files = 0;
    usize cases = 0;
    usize errorCases = 0;
    usize evaluations = 0;  ///< single evaluations + rows
    usize fmaRows = 0;      ///< rows of cases marked "fma"
    usize bytecodeHashes = 0;
    std::vector<std::string> failures;
};

/// Runs every *.jsonc file of `dir` (sorted by name).
CorpusStats runCorpus(const std::string& dir);

} // namespace helios::hxl::test
