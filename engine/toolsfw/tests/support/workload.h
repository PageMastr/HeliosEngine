#pragma once
// ED-1 workload: deterministic random transactions on the Frigate hull (sample.ship.ShipHullDef).
// Every value is an exact binary fraction and every choice comes from a seeded Xoshiro256 through
// integer arithmetic, so a seed yields byte-identical JSONC on every compiler and CRT.

#include "helios/core/random.h"
#include "helios/toolsfw/framework.h"

namespace helios::tf::test {

class Workload {
public:
    struct Options {
        bool undoRedo = true;   ///< Mix undo and redo steps in.
        bool invalid = true;    ///< Occasionally attempt edits the pre-commit hook must reject.
        bool mergeKeys = true;  ///< Occasionally run a continuous gesture (same mergeKey).
    };

    Workload(u64 seed, Options options) noexcept : m_rng(seed), m_options(options) {}

    /// Runs one random step against `doc`. True when a transaction was committed (a Do, Undo or
    /// Redo); false when the step was rejected by design (and changed nothing).
    Result<bool> step(Framework& fw, const DocId& doc);

    u64 attempts() const noexcept { return m_attempts; }
    u64 rejected() const noexcept { return m_rejected; }

private:
    Result<bool> doEdit(Framework& fw, const DocId& doc, u32 kind);
    u32 below(u32 n) noexcept { return uniformU32Below(m_rng, n); }

    Xoshiro256 m_rng;
    Options m_options;
    u64 m_attempts = 0;
    u64 m_rejected = 0;
    u64 m_counter = 0;
};

} // namespace helios::tf::test
