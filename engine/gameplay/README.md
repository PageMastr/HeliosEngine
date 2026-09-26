# engine/gameplay — the gameplay kernel (v0)

`helios::gameplay` (CMake target `helios::gameplay`, L4, HEADLESS; tests `gameplay_tests`) is the
native core of 06's gameplay framework, delivered by WP-0.19: tags (06 §1.1), attributes and
modifiers with EVE Dogma operator order and stacking penalties (06 §1.2), the item record checks
(06 §2) and the reason-code registry (06 §4). Formulas are HXL ([engine/hxl](../hxl)). Effects,
abilities, cues and the ASM arrive with WP-1.15.

| Header | Contents |
|---|---|
| `helios/gameplay/tags.h` | `TagRegistry` (+ `Builder`, `fromRecords`), `TagQuery`, `TagBits`, `TagContainer`, `TagCount` |
| `helios/gameplay/attributes.h` | `AttributeLayout`, `AttributeSet`, `Modifier`, `ModifierHandle`, `BoundFormula`, `instantiateModifier`, `resolveAttributes`, `stackingPenalty` |
| `helios/gameplay/items.h` | `validateItemDef`, `validateItemInstance` |
| `helios/gameplay/economy.h` | `ReasonCodeRegistry`, `ReasonCodeInfo` |

## Records

The kernel records live in [`schemas/gameplay`](../../schemas/gameplay) and are compiled by
helios-schemac (`helios_schema()` in this module's CMakeLists) into C++ (`gameplay/*.gen.h`,
namespace `helios::gameplay`) and Go (`services/pkg/hxl/gamedef`); the lock is
`schemas/gameplay/schema.lock.jsonc`.

| File | Records and types |
|---|---|
| `tags.hschema` | `TagDef`, `Audience` |
| `attributes.hschema` | `AttributeDef`, `AttributeSetDef`, `ModifierDef`, `ModOp`, `Magnitude`, `AttrOrConst`, `MultiplierMode`, `Quantization`, `ModifierTarget` |
| `effects.hschema` | `EffectDef` (06 §1.3: duration variant, modifiers, executions, tags, stacking, cues) |
| `items.hschema` | `ItemDef`, `ItemInstance` (`@store(ledger)`), sockets, containers, ports, decay, owner and location refs |
| `economy.hschema` | `ReasonCodeDef`, `ReasonKind`, `FaucetCap` |

Differences from 06's illustrative snippets: `AttributeDef.id` (the HXL/modifier identifier; 06 only
has the display `name`), `ModifierDef.priority` (06's formula says "highestPriority" but the record
had no priority), and `Name` placeholders where the referenced record type does not exist yet
(`UnitRef`, `AbilityRef`, `SlotRef`, `InsuranceClassRef`, `LifetimePolicyRef`, plug sets).

## Tags (06 §1.1)

```cpp
auto tags = *TagRegistry::fromRecords(tagDefs, queryTextsFromContent);   // shared_ptr<const TagRegistry>
TagContainer held(*tags);
held.add(tags->find("State.Debuff.Stun"));                               // refcounted
held.has(tags->find("State.Debuff"));                                    // parent-of: true
held.hasExact(tags->find("State.Debuff"));                               // false
auto q = *tags->compileQuery("all(State.Debuff) none(State.Immune) any(Ship.Class.Frigate, Ship.Class.Cruiser)");
q.matches(held);
```

- **Deterministic ids.** Every tag and its implied ancestors are sorted byte-wise and numbered in that
  order (u16 `TagIndex`). Segments use `[A-Za-z0-9_]`, which all sort after `.`, so a tag's
  descendants are the contiguous range `(index, subtreeEnd)`: "is X under T" is two compares, and ids
  depend only on the set of names. `hash()` (names, audiences) is part of the content version.
- **Hot set.** Tags named by `TagQuery` content (at most 1,024) get a bit in `TagBits` (128 B);
  adding a tag sets its bit and its hot ancestors' bits. A compiled query is three masks:
  `(b & all) == all && (!any || b & any) && !(b & none)`. Tags outside the hot set are answered by a
  binary search over the sorted explicit tags (the "cold path"); a 3,000-case property test checks
  both paths against a brute-force reference.
- **Replication.** `explicitTags()` (sorted `TagCount`s) replicate as index deltas (`diff()`);
  receivers `assign()` and rebuild the bits. `version()` changes when the set of held tags changes.

## Attributes and modifiers (06 §1.2)

```cpp
auto layout = *AttributeLayout::fromRecords(attributeRecords, tags);    // shared per record template
AttributeSet ship(layout);                                              // one per entity (AttrBlock)
ship.setBase(layout->find("Hull.Hp"), 4200);
auto h = *ship.addModifier(Modifier::constant(dmg, ModOp::PostPercent, 10.0, /*sourceId=*/effectId));
ship.recompute(&held);                                                  // dirty attributes only
ship.value(dmg);                                                        // final value
ship.removeModifiersFromSource(effectId);
```

The normative aggregation is in `attributes.h`. In short: operator stages in Dogma order
(`PreAssign, PreMul, PreDiv, ModAdd, ModSub, PostMul, PostDiv, PostPercent, PostAssign`), EVE
stacking penalties `S(i) = e^{-(i/2.67)^2}` per penalty group with bonuses and maluses ranked
separately by |f − 1|, the GAS `AdditiveBonus` mode, priority-based assignments, then clamps by
constant or by another attribute. Every stage sorts its values in IEEE total order before folding,
so **results never depend on the order modifiers were added** (replicas that receive modifiers in
another order agree bit for bit), and empty stages are skipped so exact values (and −0) pass
through. `S(i)` uses `helios::det::exp` and is bit-identical with HXL's
`exp(-((i / 2.67) ^ 2))`, which the corpus pins.

- **Magnitudes.** Constants; live reads of another attribute of the entity (× coefficient); live HXL
  formulas over `self` (with the modifier's `stacks`/`level`). `instantiateModifier()` turns a
  `ModifierDef` into a `Modifier`, capturing curve, `Source` and `Target/Snapshot` magnitudes and
  compiling tag requirements. Only the `Self` domain is resolved in Phase 0.
- **Derived attributes** (`AttributeDef.derived`) compute the base from other attributes and tags of
  `self`; they cannot use context fields, curves, `stacks()` or `level()`.
- **Dependencies.** Derived formulas and attribute clamps are static edges (cycles fail
  `AttributeLayout::build`); live magnitudes add refcounted dynamic edges (a cycle fails
  `addModifier` and leaves the set unchanged). `recompute()` walks a deterministic topological order
  and marks an attribute's dependents dirty only if its final value changed bitwise. Tag requirements
  and tag-reading formulas are re-evaluated after `onTagsChanged()`.
- **Replication.** `finalValues()` + `changedBits()` (per-attribute `Audience` in the spec) and
  `forEachModifier()` views (source id, op, resolved magnitude, active) for the owner's UI.
- **Parallel resolve.** `resolveAttributes(sets, tags, &jobs)` recomputes sets in chunks of 64 on the
  job system; the result is identical to a sequential resolve (tested).
- NaN magnitudes disable their modifier (`nanMagnitudes()`); NaN finals are canonical.

**Budgets (06 §12.2 GP-1)**, asserted in optimized builds (`test_attributes_perf.cpp`, best of N):

| Budget | Measured (GCC 13 Release, shared 4-core container) |
|---|---|
| 300-modifier ship recompute ≤ 50 µs | ~5.5 µs |
| one incremental change ≤ 5 µs | ~0.05 µs |
| 10k entities × 40 attributes at 5 % dirty ≤ 1 ms on 8 workers | ~5 ms on 1 thread (asserted ≤ 8 ms = 8 workers × 1 ms), ~1.4 ms on 4 threads |

**Determinism.** `test_attributes.cpp` builds 200 random ships (40 attributes with derived values,
clamps and ~150 modifiers of every kind), checks each final value against an independent
`long double` reference within 1e-9 and pins an FNV-1a hash of all final bits (`0x1243442c7d350ee5`,
GCC 13 and Clang 18 identical; MSVC and clang-cl are checked by CI). If it fails on one toolchain
only, fix the build flags (FP contraction), never the constant.

## Items (06 §2) and reason codes (06 §4)

`validateItemDef` checks stack sizes, volume and mass, tags, slot arrangements, per-instance state
(sockets, containers and decay make an item unstackable), container/port/decay values and tag
queries; `validateItemInstance` checks quantity and durability against the definition.
`ReasonCodeRegistry` indexes `ReasonCodeDef`s, enforces the class prefix (`Faucet.*`, `Sink.*`,
`Transfer.*`), caps only on faucets and non-negative limits, and answers `systemAccount()`
(`mint:<code>` / `burn:<code>`) and `mayTouch(code, account)`. The Go side has the same rules in
`services/pkg/hxl` (`ValidateReasonCode`).

## Threading

`TagRegistry`, `AttributeLayout`, `BoundFormula`, `ReasonCodeRegistry` are immutable after build and
shared by `shared_ptr`. `TagContainer` and `AttributeSet` are per-entity values with one writer;
different sets can be recomputed concurrently.

## Known limitations (Phase 0 scope)

- Cross-entity modifier domains (`ModDomain` other than `Self`, target filters) and live
  source-entity magnitudes are Phase 2 (`Unsupported`).
- schemac does not yet emit slot constants for hot attributes, cook `.htags` files or compute the hot
  set from content; callers pass query texts to `TagRegistry::fromRecords`.
- `EffectDef` is schema only; the effect runtime (timing wheel, stacking, prediction) is WP-1.15.

## Plan conformance

Plan-Rev: 6

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7. No conformance delta is open; see §5.10.4 (c) there.
