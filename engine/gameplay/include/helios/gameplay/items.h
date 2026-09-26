#pragma once
// Item definitions (06 §2): validation of ItemDef records and ItemInstance ledger rows. Phase 0
// ships the schema and these checks (the cook and T28 run them); inventories, containers, sockets
// and fitting build on them in WP-1.x/2.x.
//
// Threading: pure functions.

#include "gameplay/items.gen.h"
#include "helios/core/result.h"
#include "helios/gameplay/tags.h"

namespace helios::gameplay {

/// Checks an ItemDef: stackMax >= 1; finite, non-negative volume and mass; valid (and, with a
/// registry, declared) tags; non-empty arrangements of distinct valid slot names; items with
/// per-instance state (sockets, container, decay) are not stackable; container and decay values
/// are sane; tag queries (equip requirement, container and slot filters) compile.
Result<void> validateItemDef(const ItemDef& def, const TagRegistry* tags = nullptr);

/// Checks an ItemInstance against its definition: 1 <= quantity <= stackMax, durability within
/// [0, maxDurability] (1 without decay), rolled attributes finite.
Result<void> validateItemInstance(const ItemInstance& item, const ItemDef& def);

} // namespace helios::gameplay
