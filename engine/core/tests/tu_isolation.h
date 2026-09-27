#pragma once
// A probe for an MSVC-only hazard, shared by test_tu_isolation_a.cpp and test_tu_isolation_b.cpp.
//
// doctest names each test function DOCTEST_ANON_FUNC_<n>, numbered per file, so the test files of one
// executable have functions of the same names. cl (MSVC, not clang-cl) names a lambda local to such a
// function after that name alone (<lambda_3>@?1??DOCTEST_ANON_FUNC_2@@YAXXZ@), and it emits a static
// member variable template instantiated with the lambda's type, such as Job::kInlineOps<Fn>, as an
// external COMDAT. Two files then define a symbol of the same name, the linker keeps one, and one
// file's Job runs the other file's lambda: that is how "jobs: higher priorities run first" hung (#15).
// Class template members keyed on the lambda stay internal; the variable template does not. Names in
// an unnamed namespace carry a per-file hash (?A0x...), which is why every test file keeps its tests in
// one (tools/lint/test_namespaces.cmake).
//
// The two probe files are the same line for line down to their probe test, so both probe tests get
// the same doctest number. Each one runs a lambda returning its own tag through Keyed::kOps and a
// lambda through a real Job, and checks that its own lambdas ran; without the unnamed namespaces,
// MSVC 14.44 fails file B's checks with file A's 'A'. siteA() and siteB() let a third test check that
// the numbers still match, so an edit to one file cannot silently turn the probe into a test of nothing.
// Both probe tests must also hold the same lambdas in the same order: cl numbers a function's lambdas
// <lambda_1>, <lambda_2>, ..., so a lambda added to one of them renames the rest and they stop colliding.

#include <utility>

#include "helios/core/jobs.h"

namespace helios::tu_isolation {

/// Operations for one callable type.
struct Ops {
    char (*run)(const void* fn);
};

/// One Ops per callable type in a static member variable template, the way helios::jobs::Job keeps
/// kInlineOps. Stateless; any thread.
struct Keyed {
    template <class Fn>
    static constexpr Ops kOps{[](const void* fn) -> char { return (*static_cast<const Fn*>(fn))(); }};
};

/// Calls `fn` through Keyed::kOps<Fn>. The table is reached through a volatile pointer, so the
/// compiler cannot fold the call into an inlined direct one: the linker's copy of the table decides
/// which code runs. Any thread.
template <class Fn>
char runKeyed(const Fn& fn) {
    const Ops* volatile ops = &Keyed::kOps<Fn>;
    return ops->run(&fn);
}

/// Runs `fn` as a helios::jobs::Job, invoked through a pointer the compiler cannot see through, so
/// the call goes through the Job's kInlineOps<Fn>. Runs on the calling thread.
template <class Fn>
void runAsJob(Fn&& fn) {
    jobs::Job job(std::forward<Fn>(fn));
    jobs::Job* volatile opaque = &job;
    opaque->invoke();
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
