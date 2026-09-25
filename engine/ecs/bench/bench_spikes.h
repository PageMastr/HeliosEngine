#pragma once
// Phase 0 spikes run by ecs_bench (results recorded in engine/ecs/SPIKES.md).

namespace bench {

/// (a) mimalloc v3 heap thread affinity: allocation paths on job-system workers, cross-worker frees.
void runHeapSpike(bool quick);
/// (b) flecs DontFragment vs fragmenting storage for high-churn relationships and components.
void runFragmentSpike(bool quick);

} // namespace bench
