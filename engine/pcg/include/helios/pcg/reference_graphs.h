// helios/pcg/reference_graphs.h — graphs shared by the conformance corpus, the tests and the WP-0.9c
// throughput spike.
//
// Threading: pure functions returning new graphs.
#pragma once

#include "helios/pcg/terrain_graph.h"

namespace helios::pcg {

/// The WP-0.9c reference graph (03 §5.5a): 40 nodes shaped like its T04 reference preset for a
/// 1,500 km body — warped continents, masked ridged mountains, hills, plains, fine detail, a
/// crater-like field (inverted ridges behind a noise mask), clamped terrace plateaus and an
/// erosion-like ridged layer — with 10
/// generators totalling 80 octaves plus two domain warps (86 noise evaluations per sample at full
/// detail). Phase 0 has no dedicated crater or terrace nodes; the shapes are approximations built
/// from 02 §5.8's Phase 0 node set, and the costs are what the spike measures.
TerrainGraph makeReferenceGraph40();

/// One small graph per node kind (constant, noise, fbm, ridged, warp, add, sub, mul, min, max,
/// clamp, remap, select) for the per-node corpus. `kind` must be < NodeKind::Count.
TerrainGraph makeSingleNodeGraph(NodeKind kind);

} // namespace helios::pcg
