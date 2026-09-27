#pragma once
// TEMPORARY experiment variant of the probe (see the commit message): three ways of keying a
// template on a test lambda, to see which ones MSVC merges across test files.

#include "helios/core/jobs.h"

namespace helios::tu_isolation {

// (1) A static data member of a class template (merged: no, per run 36282920466).
template <class Fn>
struct KeyedMember {
    static char run(const void* fn) { return (*static_cast<const Fn*>(fn))(); }
    static constexpr char (*kRun)(const void*) = &run;
};
template <class Fn>
char runKeyedMember(const Fn& fn) {
    char (*const* volatile entry)(const void*) = &KeyedMember<Fn>::kRun;
    return (*entry)(&fn);
}

// (2) A static member variable template whose initializer's lambda calls the callable, laid out
// like helios::jobs::Job's kInlineOps.
struct Ops {
    char (*run)(const void* fn);
};
struct Keyed {
    template <class Fn>
    static constexpr Ops kOps{[](const void* fn) -> char { return (*static_cast<const Fn*>(fn))(); }};
};
template <class Fn>
char runKeyed(const Fn& fn) {
    const Ops* volatile ops = &Keyed::kOps<Fn>;
    return ops->run(&fn);
}

// (3) helios::jobs::Job itself, invoked through a pointer the compiler cannot see through.
template <class Fn>
void runJob(Fn&& fn) {
    jobs::Job job(static_cast<Fn&&>(fn));
    jobs::Job* volatile opaque = &job;
    opaque->invoke();
}

struct Site {
    int counter;
    int line;
};
Site siteA();
Site siteB();

} // namespace helios::tu_isolation
