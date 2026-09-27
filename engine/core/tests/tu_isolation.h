#pragma once
// A probe for an MSVC-only hazard, shared by test_tu_isolation_a.cpp and test_tu_isolation_b.cpp.
//
// doctest names each test function DOCTEST_ANON_FUNC_<n>, numbered per file, so the test files of one
// executable have functions of the same names. cl (MSVC, not clang-cl) mangles a lambda local to such
// a function with nothing unique to the file, so a template instantiated with the lambda's type gets
// the same COMDAT name in two objects, and the linker keeps one copy: one file then runs the other
// file's lambda. That is how "jobs: higher priorities run first" hung (#15). Names in an unnamed
// namespace are unique per file on MSVC, which is why every test file keeps its tests in one
// (enforced by tools/lint/test_namespaces.cmake).
//
// The two probe files are the same line for line down to their probe test, so both probe tests get
// the same doctest number, and each checks that the lambda it passes to runKeyed() is the one that
// runs. siteA() and siteB() let a third test check that the numbers still match, so an edit to one
// file cannot silently turn the probe into a test of nothing.

namespace helios::tu_isolation {

/// A per-callable-type operations table, built the way helios::jobs::Job builds kInlineOps (the
/// table that was merged in #15). Stateless; any thread.
template <class Fn>
struct KeyedOps {
    static char run(const void* fn) { return (*static_cast<const Fn*>(fn))(); }
    static constexpr char (*kRun)(const void*) = &run;
};

/// Calls `fn` through KeyedOps<Fn>. The entry is read through a volatile pointer, so the compiler
/// cannot fold the call into an inlined direct call: the linker's copy of the table decides which
/// code runs, as it did for Job. Any thread.
template <class Fn>
char runKeyed(const Fn& fn) {
    char (*const* volatile entry)(const void*) = &KeyedOps<Fn>::kRun;
    return (*entry)(&fn);
}

/// The __COUNTER__ and __LINE__ values just before a probe file's probe test, which doctest derives
/// the test function's name from (__COUNTER__ on most compilers, __LINE__ on Clang 22 and later).
struct Site {
    int counter;
    int line;
};

/// Defined by test_tu_isolation_a.cpp and test_tu_isolation_b.cpp. Any thread.
Site siteA();
Site siteB();

} // namespace helios::tu_isolation
