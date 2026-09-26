# 06 — Gameplay Framework

> **Status:** draft v5. Round 1 added the Go FMA rules, owned NPCs, account progression, interaction,
> emotes, map data and weather. Round 2 added bit-exact FBW control (§8.2); the activity, encounter,
> lockout, PvP match and matchmaking framework (§6.6–6.13); and EVA (§8.2a). Round 3 added ground vehicles
> and mounts with the `Drive` channel and their prediction contract (§8.1a, GP-4d); player-fleet mechanics
> (§8.3a, GP-15); `Restyle` and `SetLivery` for the Customization terminal (§10); and `Authority.Adopted`
> for scripts after handoff, migration or recovery (§11). Round 4 gave housing, cities and territory owners
> and acceptance: ledger plots and lots, the upkeep chain on the Industry service, cities as the Foundation
> `CityGovernance` world script projected into KV `WORLD`, and the territory lifecycle on the World State
> service with `PreProvision` hints (§9.2–9.3, GP-16, GP-17). From 04's round-4 fixes, followers restore from
> the owner's replicant in every persistent world zone (§7.3).
> **Conforms to:** ADR-001a, -002, -004 to -008, -010, -012, -013 and -014.
> **Citations:** `R09-A14` = research 09 §A14. `R09-G5` = research 09 "Requirements → Gameplay" P0
> item 5. `R02-Q10` / `R01-Q7` = the numbered requirements in §9 of research 02 / §11 of research 01.
> R03, R04 and R05 use their own IDs (`R03-P0-11`). `.hschema` snippets are illustrative;
> 02-engine-runtime owns the grammar.

## 0. Principles

1. **A kernel, not a genre (R09-A0).** The engine ships generic C++ systems. Every rule that
   belongs to one game, such as what a frigate is or how a stun works, is a **record**, a **graph**
   or a **Luau module**. Combat style, progression and death penalty change without an engine
   rebuild.
2. **Five tiers of logic.** Each rule lives in the cheapest tier that can express it.

| Tier | Contents | Runs on | Authored as |
|---|---|---|---|
| T1 Native | Attribute aggregation, effect timers, ability state machines, damage, flight control, sensors, AI tick | Client + cell | C++ (`engine/gameplay`, L4, HEADLESS-safe) |
| T2 Records | All definitions | Everywhere; clients get only shared/`client` parts | `.hschema` records |
| T3 HXL | Pure formulas | C++ and Go interpreters | Text or expression graph |
| T4 ASM | Predicted, rollback-safe abilities | Owner client + cell | Ability graph compiled to native format |
| T5 Luau | Unpredicted logic, UI, minigames | Cell (authoritative), client (presentation) | Text or logic/quest graphs |

3. **Split authority by latency tolerance (R05 §1).** The owner client predicts its own avatar,
   ship and abilities. The cell owns the world simulation and durably checkpointed quest and event
   state. The Go ledger owns value, written synchronously. Durable timers own long-running jobs.
4. **Split-authority programming model from day one (R04 §9.4).** Interactions across authority
   groups are idempotent messages to the owner (`ApplyEffect`, `ApplyDamage`, `RequestInteract`).
   Scripts hold generational handles, never pointers.
5. **One library, many hosts.** `engine/gameplay` links into the client, cell server, editor and a
   `helios-fitsim` CLI (R09-A14). Go services use the record database and a Go HXL interpreter.

## 1. The gameplay kernel

### 1.1 Tags

- **Declaration and interning.** Hierarchical tags (`State.Debuff.Stun`, `Ship.Class.Frigate`)
  are declared in `.htags` files or inline in records, each with a replication audience.
  `helios-schemac` interns each tag to a dense u16 `TagIndex` with a precomputed ancestor list.
  It also computes a **hot set**: every tag named by any `TagQuery`, at most 1,024. The registry
  hash is part of the content version.
- **Runtime storage.** `TagCounts` is a sorted `(TagIndex, refcount)` vector, because several
  effects can grant the same tag. `TagBits` is a hot-set bitset of at most 128 B. Adding tag X sets
  X's bit and the bits of its ancestors.
- **Queries.** `TagQuery{all, any, none}` compiles to three masks:
  `(b&all)==all && (any==0 || b&any) && !(b&none)`. That is 16 word operations. Tags outside the
  hot set fall back to walking `TagCounts`.
- **Replication** sends replicated explicit tags as index deltas, and receivers rebuild the bits.
- **Timed tags** are effects that grant a tag for a duration, which covers R09-A16's timers:
  crimewatch, cooldowns, i-frames.

### 1.2 Attributes and modifiers (Dogma + GAS)

```
record AttributeDef @table("attr") {
  id: Name;                                                // identifier for HXL and slot constants: attr(e, Shield.Max)
  name: LocString; unit: UnitRef; default: f64;
  minClamp: AttrOrConst?; maxClamp: AttrOrConst?;          // Shield.Current <= Shield.Max
  derived: HxlExpr?;                                       // EffectiveHP = f(hp, resists)
  multiplierMode: enum { Product, AdditiveBonus } = Product;   // Dogma vs GAS summing
  stackingPenalised: bool = false;
  replicate: Audience = server; quant: Quantization?; persistBase: bool = false;
}
record AttributeSetDef @table("aset") { attrs: list<AttrRef>; }   // one per record template
```

**Storage.** Each entity's `AttrBlock` component (layout from its record template's attribute set) stores `base[]`, `final[]`, a dirty bitset and
aggregator heads in SoA form. Designers add attributes without writing code. Attributes that native
code reads every tick (`Ship.MaxLinearSpeed`) get schemac-generated slot constants, so reading them
needs no lookup.

**Modifiers.** A modifier is `{attr, op, magnitude, priority, penaltyGroup, exempt, requirement: TagQuery?, source}`.
`priority` picks the `PreAssign` and `PostAssign` winner below (ties: the larger value).
- **Magnitude sources:** a constant, a level curve, a source or target attribute (captured at apply
  or read live), or HXL.
- **Operators, in Dogma order (R01 §5):** `PreAssign, PreMul, PreDiv, ModAdd, ModSub, PostMul,
  PostDiv, PostPercent, PostAssign`. GAS `Override` maps to `PostAssign`.

```
v = PreAssign ? PreAssign.highestPriority : base
v = v * Π PreMul / Π PreDiv + Σ ModAdd − Σ ModSub
F = PostMul ∪ {1 + p/100 : PostPercent} ∪ {1/d : PostDiv}
AdditiveBonus: v *= 1 + Σ(f − 1)                              // GAS: two 1.5× ⇒ 2.0×
Product:       per penaltyGroup, bonuses and maluses each sorted by |f−1| desc,
               v *= Π_i (1 + (f_i − 1)·S(i)),  S(i) = e^{−(i/2.67)²}, S ≡ 1 if exempt  // R09-A0
v = PostAssign ? PostAssign.highestPriority : v;   final = clamp(v, minClamp, maxClamp)
```

- **Dependency graph.** Derived attributes and live-read magnitudes add edges, and schemac rejects
  cycles. Dirty flags propagate topologically. The per-tick `AttributeResolve` job recomputes only
  dirty attributes, at O(modifiers on that attribute).
- **Cross-entity targeting (Dogma "location" modifiers).** A modifier targets
  `{domain: Self|Owner|Pilot|Ship|ShipModules|Fleet|Grid|AreaVolume|Followers, filter: TagQuery}`, so
  one skill can modify every module tagged `Item.Weapon.Hybrid`, or every drone the pilot launches
  (§7.3).
- **Brain snapshot (R01 §3.2).** A character's aggregated modifiers form a serialized
  `CharacterBrain` that travels with it on handoff instead of being recomputed.

**HXL (Helios eXpression Language).** HXL is pure, deterministic stack bytecode compiled by
schemac. Formulas are data, not C++ (R09-A0).
- **Built-ins:** arithmetic, `min max clamp lerp select`, `pow exp ln sqrt asinh` from the
  deterministic `hmath` library (not libm), `curve(t, x)`, `attr(e, A)`, `tag(e, T)`, `stacks()`,
  `level()`, and context fields.
- **Interpreters.** There are two, each about 1.5k lines: C++ (`engine/hxl`) and Go (`pkg/hxl`, 05 §8,
  used by the Industry service for job durations and fees, vendor price checks and crew-mission
  timers). A golden conformance corpus must produce bit-identical f64 results on MSVC, GCC, Clang and
  Go.

```
formula TurretHitChance(src, tgt, ctx) =                       // R09-A2, as data
  0.5 ^ ( (ctx.angularVelocity * 40000 / (attr(src,TrackingSpeed) * attr(tgt,SignatureRadius)))^2
        + (max(0, ctx.distance - attr(src,OptimalRange)) / attr(src,FalloffRange))^2 )
```

**Cross-language float rules** (normative for `engine/hxl`, `det::`/`hmath`, `pkg/hxl` and its
`pkg/hxl/det` port). C++ builds these units with `-ffp-contract=off` and `/fp:precise`, with no
fast-math (02 §2.1). MSVC (VS 2022 and VS 2026, both ADR-001a toolsets) contracts only under `/fp:contract`, which is banned. Go needs more
care. Its spec lets the compiler fuse `x*y + z` into one FMA, "possibly across statements". gc does
this on arm64 and the other non-x86 ports, and on amd64 at `GOAMD64=v3` or higher. Go also folds
untyped constant expressions in exact arithmetic, where C++ rounds every step to f64. A
performance-motivated build flag could therefore silently change industry durations or vendor
prices. The rules:
1. **Every float multiplication in `pkg/hxl/...` is the direct operand of an explicit conversion**,
   `float64(a*b) + c`. The spec defines an explicit conversion as a rounding point that forbids
   fusion. This includes every Horner step of the `det` port and products whose result is stored in a
   local first.
2. **No constant arithmetic in the port.** Constants are single typed hex-float literals generated
   from the C++ tables by `helios-tool hxl-gen-consts`, never expressions such as `2*math.Pi`.
3. **Banned:** `math.FMA` and the libm-style functions (`Exp`, `Log`, `Pow`, `Asinh`, `Sin` and so
   on). **Allowed:** `math.Sqrt`, `Abs`, `Floor`, `Ceil`, `Trunc`, `Copysign` and `Float64bits`, which
   are exact or correctly rounded on every target.
4. **The Go VM never constant-folds, reassociates or fuses opcodes.** It runs schemac's bytecode op by
   op, and each op stores an f64 into the stack slice.
5. **Lint.** The `hxlfloat` analyzer (`go/analysis`, run by `go vet -vettool` in CI and in the
   pre-commit hook) flags an unconverted float `*`, any float constant expression with more than one
   literal, and any banned `math` call in `pkg/hxl/...`. It has a self-test with planted violations.
6. **Pinned build.** Services build with `GOAMD64=v1`, set by `helios-dev`, the build scripts, the
   Dockerfiles and CI. `helios-backend` reads `GOAMD64` from its `debug.BuildInfo` at start. It logs
   the value and refuses to start in production unless the value is `v1`. This is defence in depth;
   rules 1–5 are the guarantee.
7. **Corpus sensitivity.** The corpus includes ≥ 1,000 **FMA-sensitive vectors**, inputs found by
   search where the fused and unfused results differ. CI runs it on `windows/amd64`, on `linux/amd64`
   at both `GOAMD64=v1` and `v3`, and on `linux/arm64` (a native arm64 runner), and compares each run
   with the MSVC, GCC and Clang results.

### 1.3 Effects

```
record EffectDef @table("eff") {
  duration: variant { Instant; Duration{secs: Magnitude}; Infinite;
                      Periodic{period: Magnitude; secs: Magnitude?; executeOnApply: bool} };
  clock: enum { Zone, Wall } = Zone;            // Zone = dilated (TiDi-safe)
  modifiers: list<ModifierDef>; executions: list<ExecutionRef>;   // native or HXL
  grantedTags: TagSet; grantedAbilities: list<AbilityRef>;
  applicationReq: TagQuery; ongoingReq: TagQuery;   // ongoing unmet => inhibited, not removed
  removeEffectsWithTags: TagSet; immunityTags: TagSet;
  stacking: { policy: enum{None, BySource, ByTarget}; limit: u16; refreshDuration: bool;
              resetPeriod: bool; onExpire: enum{ClearAll, RemoveOneAndRefresh}; overflow: EffectRef?;
              keep: enum{Newest, Strongest} = Newest };   // Strongest: command bursts (§8.3a)
  cues: list<CueBinding>; predictable: bool = false; persist: bool = false; publicIcon: bool = false;
}
```

- **Runtime state.** An `ActiveEffect{handle, def, source, capturedContext, level, stacks,
  startTick, endTick, nextPeriodTick, predictionKey}` lives in the target's `EffectList`.
- **Timing.** Expiry and periodic ticks go into a hierarchical **timing wheel on zone ticks**, so
  time dilation slows every buff and cycle consistently (R01-Q3). `Wall` effects store an absolute
  expiry.
- **Persistence.** `persist` effects (wounds, long buffs, clone penalties) save with the character.
  Resistances and immunities are ordinary attributes and tags.

### 1.4 Abilities and the native Ability State Machine

```
record AbilityDef @table("abl") {
  tags: TagSet; activationOwnerReq: TagQuery; activationTargetReq: TagQuery;
  blockedBy: TagSet; cancelsAbilitiesWith: TagSet;
  costs: list<CostDef>; cooldown: { effect: EffectRef; group: TagRef? };   // cooldown = timed tag
  charges: { max: u8; rechargeSecs: Magnitude }?;
  cycle: { secs: Magnitude; autoRepeat: bool; reload: ReloadDef? }?;       // EVE module cycles
  activation: enum { Instant, Cast, Channel, Toggle, ChargeUp };
  targeting: TargetingRef; resolution: ResolutionRef; range: Magnitude?; requiresLOS: bool;
  implementation: variant { Graph{asm: AsmRef}; Luau{module: ScriptRef}; Native{id: Name} };
  prediction: enum { Predicted, ServerOnly };  queueWindowMs: u16 = 400; gcdGroup: TagRef?;
}
```

All gating is native: requirements, costs, cooldowns, charges and the `CanHarm` check (§9.4). Only
the body varies. **Predicted** abilities compile from the ability graph to an immutable,
content-hashed **ASM** program (ADR-002, R05-P1-12):

```
struct AsmProgram  { AsmState states[]; AsmTransition transitions[]; AsmOp ops[]; VarLayout vars; };
struct AsmInstance { u64 programHash; u16 state; u16 flags; u32 stateEnterFrame;   // POD <= 192 B
                     u32 predictionKey; EntityHandle target; i32 vars[32]; };   // snapshotted per frame
```

Ops are fixed 16-byte instructions: `Wait(frames|inputRelease|event)`, `CommitCost`,
`StartCooldown`, `ApplyEffect`, `GrantTagTimed`, `ShapeQuery`, `TraceHitscan`,
`SpawnPredictedProjectile`, `SetMovementOverride` (`Attach` to an anchor, as grabs and mounting use, or
`RootMotion` along a cooked track, as mount jumps use; §8.1a, §8.2a), `PlayMontage`, `TriggerCue`, `Branch`
and `EmitServerEvent`. `EmitServerEvent` is the only bridge to Luau, and it fires on the server after
confirmation. The compiler rejects a predicted graph that contains a Luau call, a ledger operation,
an unpredicted spawn or a wall-clock read.

**Prediction protocol** (Overwatch model, R05 §4.2):
1. **Client runs ahead.** It allocates `PredictionKey = inputSeq<<8 | slot` and runs the ASM on its
   command frame. Costs, `predictable` self-effects, cooldown tags, movement overrides and cues go
   into a **predicted overlay** stamped with the key.
2. **Cell repeats** the program on the same frame from its input buffer and replies on the owner
   channel: `{key, accepted|rejected, AsmInstance, cooldowns, costs}`.
3. **Resolve.** On accept, the overlay yields to authoritative state and cues deduplicate by key.
   On reject or divergence, the client restores movement, `AsmInstance`s, overlays and charges at
   the confirmed frame and re-simulates its buffered inputs.

Cooldowns and periodic self-effects run on command frames inside the rollback domain, so we can
predict both, which GAS cannot (R09-A0). We **never** predict damage to others, death, loot or
value. **ServerOnly** abilities (hacking, calling a mount, EVE 1 Hz module cycles) use the same
gating, then run Luau or native code on the cell while the client shows a pending state.

### 1.5 Cues

- **Records.** `CueDef` records are keyed by `Cue.*` tags. Their `client{}` block names VFX, SFX,
  decals, camera shake, haptics, shield-impact ring-buffer writes (03-rendering) and **wind
  emitters** (`windEmitter{radius, strength, falloff, secs}` for thrusters, VTOL wash and explosions,
  feeding 03 §5.6a's analytic wind). Servers never load cue payloads, and the cook enforces the split
  (R02-Q6).
- **Events** (`Execute`, `Add`/`Remove`, `Burst`) carry location, normal, magnitude, source,
  target, surface and prediction key.
- **Transport.** Unreliable, relevance-filtered and batched. In large fights cues are aggregated
  per target per tick and capped per client (the R01 fanout lesson).

### 1.6 Kernel replication and persistence

- **Attributes** replicate per `AttributeDef` (HP and signature bands to all, most ship stats to
  the owner). Only `persistBase` values persist; derived values never do.
- **Explicit tags** replicate per declaration.
- **Active effects**: the owner gets the full list, others get `publicIcon` effects only.
- **`AsmInstance`s, cooldowns and charges** go to the owner only, plus a public "casting X" state.

## 2. Items and inventory

```
record ItemDef @table("itm") {
  name: LocString; tags: TagSet; stackMax: u32 = 1; volume: f32; mass: f32;
  baseAttrs: map<AttrRef, f64>;
  stateEffects: map<ItemState, list<EffectRef>>;   // Offline/Online/Active/Overloaded (Dogma)
  arrangements: list<list<SlotRef>>;               // occupies ALL slots of ONE arrangement (SWG)
  sockets: list<SocketEntry>; equipReq: Requirement; container: ContainerSpec?; port: ItemPortSpec?;
  bind: BindRule; decay: DecaySpec?; insurance: InsuranceClassRef?; lifetime: LifetimePolicyRef;
  client { appearance: AssetRef<Prefab>; icon: AssetRef<Texture>; }
}
struct ItemInstance @store(ledger) {                       // Postgres row + JSONB (ADR-008)
  itemId: u64; def: RecordId; owner: OwnerRef; location: LocationRef; flag: u16;  // EVE tree
  quantity: u32; version: u64; fenceEpoch: u64;
  payload: { plugs; rolled: map<AttrRef,f64>; crafted: CraftStamp?; rollAudit: RollAuditRef?;
             durability: f32; customName: string?; transform: Xform? }
}
```

- **Location tree (R09-A15).** Every item has exactly one location: another item, an entity, a
  hangar or space. `ContainerSpec` sets capacity (volume and count), an allowed `TagQuery`, slot
  descriptors and access. Equipping picks the first fully free arrangement, or the one that
  displaces the fewest items. A companion's gear lives in its control device the same way (§7.3).
- **Sockets and plugs (R05 §5.1).** A `SocketEntry{type, initialPlug, randomizedPlugSet,
  reusablePlugSet, rollCount}` defines a socket. Plugs are `ItemDef`s tagged `Item.Plug.*`, and
  their `stateEffects` apply to the host item. A `PlugSetDef` is a weighted list with conditions.
  Inserting a plug is a rate-limited ledger mutation.
- **Seeded, auditable RNG.** Rolls resolve on the cell at grant time. Seed =
  `SipHash-2-4(key = shardSalt[saltId], itemId ‖ def ‖ contextHash ‖ socketIndex)`, feeding a PCG64
  stream. Seeds are never sequential, which avoids Bungie's perk-weighting bug (R05 §8.6).
  `RollAudit{saltId, algoVersion, inputsHash, outputs}` lets GM tools reproduce any roll. Loot
  tables (nested weights, guaranteed drops, pity counters, lockouts) use the same scheme, and CI
  runs chi-square and perk-pair independence tests on all of them.
- **No client authority (ADR-008).**
  - Clients send only intents, such as `MoveItem(itemId, to, flag, expectedVersion)`.
  - The cell validates range, locks, container rules and requirements, then issues a ledger
    `Execute(LedgerTx{idem, reason, fence, preconditions, ops})` (05 §1.6) on items in its
    authority group's **custody**.
  - Ledger events refresh the cell's inventory cache. Changes made elsewhere (mail, market, GM)
    invalidate it.
  - Value never goes through write-behind. The one exception is consumables marked
    `ledger_policy: batched_consume` (ammo, fuel), which ability costs spend locally and report
    every 10 s (R05-P0-8).
- **Loadouts.** Equipping applies `stateEffects` through the kernel. The pure `FittingValidator`
  checks slot type and size, CPU, grid, calibration, mass and skills, and is shared by the client
  preview, the cell and `helios-fitsim`. Saved fittings use an exchangeable `.hfit` text format.
- **Physicalized cargo (SC, Phase 5).** A container item can materialize as a Jolt-body entity with
  ledger location `(grid, entity)`, while its contents stay logical. It is gated by per-grid entity
  budgets (R04 §9.2).

## 3. Crafting and resources

- **Resource classes (R02 §6.2).** `ResourceClassDef` forms a tree
  (`Inorganic.Metal.Ferrous.<generated>`). Each class sets attribute ranges (OQ, CD, DR, HR, MA,
  PE, SR, UT, FL, CR, ER), lifetimes, pool membership (minimum, random, fixed, native), planets, a
  quality throttle that keeps about 90% of stats under a cap, and noise parameters.
- **Resource scheduler (Industry/resources service, 05 §1.8).** It checks shifts every N hours
  and refills the pools. Each new spawn gets a deterministic name, salted-seed attribute rolls with
  an audit record, a despawn time and a per-planet concentration seed. It publishes
  `resources.spawned` and `resources.despawned` events.
- **Survey and sample.** Survey is an ability: the cell evaluates `concentration(spawn, pos)` over
  a grid and returns samples. Seeds stay server-only, so clients cannot precompute hotspots.
  Sampling is a cycling ability that grants a ledger resource stack keyed by spawn.
- **Harvesters** are evaluated lazily, never ticked. From `{lastEval, rate = base × concentration ×
  mods, hopper, power, maintenance}`, accrual is computed analytically on access. Durable timers
  fire hopper-full, despawn and power-out events. Grants use the key `(harvester, evalWindow)`.

```
record SchematicDef @table("sch") {
  complexity: u16; requires: Requirement; xp: list<XpGrant>; output: ItemRef;
  slots: list<{ kind: variant{ResourceClass; ItemTag}; qty: u32; identical: bool; contribution: f32 }>;
  properties: list<{ name: Name; weights: map<ResAttr, f32>; min: f64; max: f64; ceiling: CurveRef }>;
  outputAttrs: map<AttrRef, HxlExpr>; assembly: HxlExpr; experiment: HxlExpr;
  manufacturable: bool; minigame: ScriptRef?;
}
```

**Weighted quality.** For each property p,
q_p = Σ_s c_s·Σ_a w_{p,a}·r_{s,a} / Σ_s c_s·Σ_a w_{p,a}, normalized to 0–1000. The
experimentation ceiling is `ceiling(q_p)`. The formula is a pure function, `CraftQuality`, shared
by the cell and 08's crafting panel estimate (08 §1.7.2), as `FittingValidator` is.

The **crafting session** is a server state machine:
`Open → Select → Slot → Assemble → Experiment×N → Customize → Finalize(Prototype | ManufacturingSchematic)`.
- **Slot** moves ingredients into escrow (`EscrowOpen`). A durable timer refunds them if the
  session dies.
- **Rolls** use seeded, audited streams.
- **Minigames** (Luau or graph) return bonuses that the cell clamps.
- **Finalize** is one atomic ledger transaction: consume the inputs, create the output stamped
  `CraftStamp{crafterId, serial, station, time}`, and grant XP. The crafter's name is resolved from
  `crafterId` at display time, so account erasure needs no item rewrite (05 §6.6).

**Factories and industry.**
- **Factories** run manufacturing schematics as Industry-service jobs. Every unit copies the
  prototype's stats.
- **EVE industry.** `BlueprintDef` activities (manufacture, ME/TE research, copy, invention,
  reactions) declare inputs, outputs, `duration: HxlExpr`, facility bonuses and fees (a sink). The
  cell submits the job context. The Industry service recomputes duration and fee in Go HXL within
  clamps and registers a **durable timer**. Jobs progress offline and survive restarts (R01-Q13).
  Companion crew-skill missions reuse this path (§7.3).

## 4. Economy

- **Wallets and reason codes.** `CurrencyDef` records live in double-entry ledger wallets. Every
  journal entry names a `ReasonCodeDef` (`Faucet.Bounty.NPC`, `Sink.Tax.Market.Broker`,
  `Transfer.Trade`) whose class (faucet, sink or transfer) restricts which system accounts it may
  touch. This makes faucet and sink telemetry correct by construction (R09-A8, R01-Q8).
- **NPC vendors.** A `VendorDef` sets HXL prices (which can read the regional price index,
  standings and taxes), stock and restock, standing requirements, and rotations from the live-ops
  calendar (R05-P1-16). A purchase is one idempotent ledger transaction.
- **Player vendors** (SWG) are in-world views of escrowed, structure-scoped Market listings.
- **Regional markets** (EVE order books) live in the Market service. The cell resolves the
  character's station, system and region, validates terminal access (a live `TerminalSession`,
  §9.6), and attaches a **fee context**: a `Character.BrokerFeeRate` attribute from skills and
  standings, which the service clamps to the `TaxPolicyDef` bounds.
- **Contracts and trades.** Contracts (exchange, courier, auction) live in the Market service.
  Gameplay checks courier delivery against the package's ledger location. A secure trade is one
  `AtomicSwap` with version checks.
- **Taxes.** A `TaxPolicyDef` is scoped to global, region, faction, city or structure. Owners set
  rates within designer bounds.
- **Insurance.** An `InsurancePlanDef` sets the premium (a sink), payout and claim delay. The death
  pipeline files claims, and durable timers pay them (a faucet).
- **Economy guard.** Each reason code has rate thresholds wired to telemetry anomaly rules and to
  named kill switches (`econ.trade`, 05 §1.15–1.16). Scripts move value only through declared
  reason codes with caps (§11).
- **Economy simulation hooks (Phase 4–5, R04-P2-20).** A Go Economy Sim service runs per-region
  BGS/Quantum-style ticks over ledger flows, activity events and an `EconomyModelDef` of abstract
  agents. It publishes a `RegionEconomyState` world record (price indices, scarcity, demand,
  traffic, mission weights) that vendor formulas, spawners and mission generators read. It never
  writes to the ledger.

## 5. Progression

### 5.1 Character progression

**One model.** SWG skill boxes, EVE trained skills, Destiny subclass nodes, talent trees, and (§5.2–5.3)
achievements, collections, codex, legacy and season tracks are all `ProgressionGraphDef`s (R02-Q13):

```
record ProgressionGraphDef @table("prg") {
  kind: enum { SkillTree, Talents, Achievements, Collection, Codex, Legacy, SeasonTrack } = SkillTree;
  scope: enum { Character, Legacy } = Character;   // Legacy = account × shard (05 §1.5)
  window: CalendarWindowRef?;                      // SeasonTrack and time-limited achievements
  mirrorAsEntitlement: bool = false;               // cosmetic unlocks honoured on every shard (§5.2)
  caps: map<Name, u32>; nodes: list<ProgressionNodeDef>;
}
struct ProgressionNodeDef {
  prereqs: list<NodeRef>; costs: list<CostDef>; grants: list<GrantDef>;
  exclusiveGroup: Name?; refund: RefundPolicy;
  criteria: list<CriterionDef>; mode: enum { All, Any, Sequence } = All;   // event-driven nodes
  tiers: list<{ count: u32; grants: list<GrantDef> }>; points: u16; hidden: bool; retired: bool;
}
struct CriterionDef { event: EventTypeRef; filter: TagQuery; count: u32 = 1;
  distinct: enum { None, Target, Zone, ItemDef, Poi } = None; context: TagQuery?; }
```

- **Graph contents.** Caps (such as `SkillPointCap = 250`) and nodes. Each node has prerequisites,
  costs (`Xp{type, n}`, `Points`, `Currency`, `TrainTime{HxlExpr}`), grants (permanent "skill
  mods" as `persist` effects, abilities, schematics, certification tags, titles, unlocks), an
  exclusive group and a refund policy. Event-driven nodes (achievements, codex, collections) complete
  from `criteria` instead of costs.
- **XP.** `XpTypeDef` defines typed XP (`Combat.Rifle`, `Craft.Weapons`, `Social.Music`,
  `Influence.<companion>`, `Legacy.XP`, `Season.<id>`), with caps and rest bonus. Gameplay events
  grant it.
- **Time-trained skills** queue in the Industry/Timers service and progress offline.
- **Levels and power.** `LevelCurveDef` defines level curves. Destiny-style **Power** is a derived
  HXL attribute over equipped items. `ActivityDef.sync` (§6.9) applies level sync, bolster, a power cap
  and a delta-scaling effect when a player enters an activity.
- **Factions.** `FactionDef` has a relations matrix and propagation coefficients, so hurting A
  raises standing with A's enemies (R09-A16). Standings run from −10 to +10.
  `StandingThresholdDef` grants gating tags (`Standing.Faction.X.Friendly`). Ranks and `TitleDef`
  titles are graph grants.
- **Live safety.** Records hot-reload in 2 s or less.
  - A graph version that removes or remaps nodes needs a `ProgressionMigrationDef` (remap, refund
    or grandfather), applied and logged at next login. This guards against NGE-style breakage
    (R02 §8.1).
  - Progression deltas go to the character service through the idempotent `ApplyProgression`
    call (05 §1.5). Currency purchases are ledger transactions.

### 5.2 Account progression: achievements, collections, codex and legacy

`AchievementDef`, `CollectionDef`, `CodexEntryDef` and `LegacyUnlockDef` are schemac record-template
presets over `ProgressionGraphDef` with a fixed `kind` and `scope: Legacy`. Designers get four focused
editors in T09; the runtime keeps one model.

| Kind | Reference | Nodes | Completes on | Grants |
|---|---|---|---|---|
| Achievements / triumphs | Destiny, SWTOR | Criteria with tiers and points; hidden and retired flags | `criteria` over the event index | Titles (a completed set grants a `TitleDef`, Destiny's seals), cosmetics, currency |
| Collections | Destiny | One entry per `ItemDef` tagged `Collectible.*`, species, mount, emote, livery or title | First `ItemAcquired`/`Unlocked` of the key | Unlock bit. `reacquire` pulls a fresh copy as a ledger purchase (`Sink.Collection.Reacquire`); rolled items re-roll on the current plug sets |
| Codex | SWTOR | Lore entries (client `LocString` and image) | `Discovered`, `Interacted`, `Scanned` or `Killed(filter)` | Per-planet completion XP and titles |
| Legacy | SWTOR | Perks bought with `Legacy.XP` (a 10 % share of character XP) and legacy currency | Costs, like skills | Account perks applied at login to every character on the shard (XP bonus effects), species unlocked for §10 creation, a **legacy bank** (a ledger hangar owned by `(Legacy, account)`), legacy titles |
| Map discovery | SWTOR, SWG | One entry per `MapRegionDef` and POI (§9.8); **Character** scope | `EnteredVolume` | Fog-of-war bits, codex entries |

**Runtime.**
- **Load.** At login the cell loads the account snapshot (completion bitset, counters, claim state)
  with the character.
- **Dispatch.** Incomplete, eligible nodes subscribe to §6.1's event index, the same `(eventType,
  TagIndex)` dispatch that quests use. Dispatch cost therefore does not grow with the number of defined
  achievements. `distinct` criteria keep a sorted set of ≤ 256 keys per node; larger sets are
  collections.
- **Counters.** Counters accumulate in an `AccountProgressCache` component on the character's AG and
  are checkpointed with it.
- **Flush.** The cell flushes deltas every 30 s, on node completion, and on logout or transfer, with
  `ApplyAccountProgression(account, deltas, flushSeq)` (05 §1.5). The call is idempotent by
  `(account, cell incarnation, flushSeq)`. Deltas are increments, so two sessions of one account
  commute.
- **Completion** grants rewards as ledger `Grant`s keyed `(account, node, tier)` with reason
  `Faucet.Achievement.*`. A crash loses at most one flush window of counter progress, or the owner's
  checkpoint window on a cell kill, and never double-grants.
- **Cross-shard unlocks.** A graph with `mirrorAsEntitlement` also issues an idempotent entitlement
  grant (05 §1.20) on completion, so cosmetic unlocks (emotes, titles, species, liveries) are honoured
  on every shard without a global progression service.
- **Abuse.** Luau reports progress only for `Custom` criteria and needs the `progress.account`
  capability; UGC never has it. Per-node increment-rate caps feed the trust pipeline (05 §1.16).

### 5.3 Seasons and reward tracks (G17, R05-P1-16)

```
record SeasonDef @table("ssn") {
  window: CalendarWindowRef;        // start and end published by 05 §1.15's calendar
  track: ProgressionGraphRef;       // kind SeasonTrack, scope Legacy; ranks 1..N
  xp: XpTypeRef;                    // Season.<id>, fed by SeasonXpRuleDef{event filter → HxlExpr}
  premium: EntitlementRef?;         // premium lane (05 §1.20)
  catchUp: HxlExpr;                 // XP multiplier from weeks behind the expected rank
  weeklyCap: u32?; claimWindowDays: u16 = 30;
  seasonalEffects: list<{ rank: u16; effect: EffectRef }>;   // artifact-style perks
}
```

- **Ranks.** Each rank node has free and premium grants. Premium grants check the entitlement at claim
  time.
- **Calendar-bound.** `calendar.season.start` activates XP rules and subscriptions.
  `calendar.season.end` locks the track on every cell within 60 s, applied at a tick boundary.
  `calendar.reset.weekly` resets weekly caps and bounties.
- **Claims.** Rewards are claimed in the UI or auto-claimed. When the claim window closes, earned but
  unclaimed rewards are mailed once (05 §1.9), keyed `(account, season, rank, lane)`.
- **Seasonal effects** are `persist` effects with a `Wall` expiry at season end. Seasonal vendor and
  event rotations read the same calendar (§4, §6.3).

## 6. Missions, quests, dialogue, events and activities

### 6.1 Quests

The quest graph (T12) compiles to a **server state machine in data**, with Luau only for actions and
custom predicates.
- `QuestDef` holds stages, objectives, conditions (the Conditions/Facts system shared with dialogue, AI
  and spawns), rewards, failure, timers, sharing rules and phase actions.
- Objective types: `Kill(TagQuery,n)`, `Collect`, `Goto(volume)`, `Interact`, `Escort`, `Scan`,
  `Craft`, `Deliver`, `Survive`, `Custom(Luau)`. Encounters (§6.7) add `Damage(target, pct)` and
  `Hold(volume, secs)`.
- A `QuestInstance{quest, owner: player|party|world, stage, counters, vars}` moves with the character's
  authority group.
- **Event index.** Gameplay events (`Killed`, `ItemAcquired`, `EnteredVolume`, `Crafted`,
  `Interacted`…) dispatch through an index keyed by `(eventType, TagIndex)`. Only active objectives,
  encounter phases (§6.7), match scoring rules (§6.11) and incomplete progression criteria (§5.2)
  subscribe, so dispatch cost does not grow with the number of quests or achievements.
- Rewards are ledger grants keyed by `(questInstance, stage)`, so they are never paid twice.

### 6.2 Mission generators (AO, EVE, SC)

A `MissionTemplateDef` holds parameter ranges, a site template (an object container or modular tiles,
R05-P2-21), an objective blueprint, `reward: HxlExpr` (over difficulty, distance, standings and
`RegionEconomyState`), availability and weights. Generators run on the cell per terminal or agent, with
seeded offers refreshed on a timer. Accepting an offer spawns the site through the **site spawner**:
anomaly entities with a lifetime, signature strength and template (R09-A5), found by the generic
probe-scan query. An instanced site (EVE abyssal, SC bunker) is an `ActivityDef` of kind `Expedition`
(§6.6).

### 6.3 Dynamic and public events (GW2, Destiny)

An `EventDef` graph holds phases, objectives, a participant registry, **contribution scoring** (weighted
damage, healing, interactions and presence) with reward tiers, participant-count scaling curves applied as
effects, and success/fail branches that form chains.
- A per-zone **Event Director** schedules events from the live-ops calendar, world flags and economy
  weights.
- State is checkpointed to the activity store (`ACTIVITY` KV, 05 §1.12), so a replacement cell rehydrates
  within 5 s (R05-P0-5).
- `WorldFlagDef`s (scoped to shard, zone or region; persisted; replicated) let results change spawner sets
  and vendor stock. They are stored and projected by the World State service (05 §1.21).
- Meta-events that span zones coordinate through that world-state service over NATS.

### 6.4 Dialogue, group conversations and cinematics (SWTOR, R03-P0-11)

`DialogueDef` nodes are `Line{speakerRole, lineId, stagingMeta}` (a stable `lineId` for localization and
VO), `Choice{options{paraphrase, conditions, alignmentDelta, influenceDeltas, effects}}`, `Branch`,
`Action` and `Cinematic`. A server `ConversationSession` holds eligible party members in range, role
bindings (including a `PlayerSpeaker` slot and summoned companions, §7.3), the current node, a server
timeline, a choice window (default 30 s) and a resolver: `Roll`, `Leader`, `Vote` or `Owner`.
1. Each player's own side effects (alignment, the influence of their own summoned companion, facts) apply
   to their own choice immediately.
2. **Roll:** each participant rolls d100 plus modifiers from a seeded, audited stream. The winner's option
   plays with the winner's character, voice and animation set.
3. Social points are a typed XP: 1 per roll and 2 per win, scaled by group size.
4. A line is skipped only when all participants skip it. A disconnected participant auto-passes after
   5 s, so a session never blocks on one client.

**Cinematic hooks.** `Cinematic.Play(seq, participants, blocking)` tags players `State.InCinematic`
(immune, ignored by AI). Clients play the sequence synced to server time.

### 6.5 Phasing

Actions call `Phase.Enter/Leave(layer)`. An entity's `PhaseFilter` is a TagQuery over viewer facts. The
content cook assigns each distinct filter a bit of 04 §7's 64-bit per-zone `phase_mask`; the cell sets a
viewer's bits when its facts change, and interest management tests the masks (R03-P0-6, 04 §4.3). The
editor's "view as state" debugger uses the same evaluator.

### 6.6 Activities (G12, G19; R03-P1-8, R05-P0-5/6, R05-P2-22)

An **activity** is any bounded piece of group content with an entry, a roster, a result and rewards:
Destiny strikes, raids and Crucible matches, SWTOR flashpoints, operations and warzones, EVE-style
instanced sites, and opt-in PvP risk zones. One record drives all of them. `kind` only selects a preset
of defaults, the way §5.2's presets do.

```
record ActivityDef @table("act") {
  kind: enum { Strike, Flashpoint, Raid, Scenario, PvPMatch, Arena, RiskZone, Expedition };
  zone: ZoneTemplateRef; tickProfile: TickProfileRef;          // an activity instance (04 §7); 30 or 60 Hz
  roster: { min: u8; max: u8; teams: u8 = 1; partyMax: u8;      // strike 1–3; flashpoint 1–4; op 8/16; 6v6 = 2 × 6
            roles: list<{ role: TagRef; min: u8; max: u8 }> };  // Role.Tank/Healer/Damage; empty = roleless
  entry: { requirement: Requirement; minLevel: u16?; minPower: u32?; quest: QuestRef?;
           key: { item: ItemRef; consume: enum { OnLaunch, OnFirstEncounter } }?; zoneRules: ZoneRulesRef };
  difficulties: list<DifficultyTierDef>;                        // §6.9
  modifiers: { pool: list<ActivityModifierRef>; rotation: CalendarWindowRef?; picks: u8 };   // §6.9
  sync: SyncPolicy; scaling: { perPlayer: CurveRef?; solo: bool = false };                  // §6.9
  flow: variant { Linear{ encounters: list<EncounterRef> };
                  Graph{ nodes: list<{ encounter: EncounterRef; after: list<u8> }> } };      // §6.7
  lockout: LockoutPolicyRef?; checkpointSource: enum { None, Leader, Intersection } = None;  // §6.8
  matchmaking: { mode: enum { PremadeOnly, Matchmade, Both }; queue: QueueDefRef?;
                 backfill: BackfillPolicy; joinInProgress: bool = true };                    // §6.10, §6.13
  rosterPolicy: { afk: AfkPolicy; voteKick: VoteKickPolicy; leaver: LeaverPolicy };        // §6.10
  revive: RevivePolicy;                                         // default for every encounter, §6.7
  match: MatchRulesRef?;                                        // PvP and mixed modes, §6.11
  rewards: { completion: list<GrantDef>; firstClearPerPeriod: list<GrantDef>; lootRights: LootRightsRef };
  timeLimitSecs: u32?; abandonSecs: u16 = 120; closeGraceSecs: u16 = 60;
  netcode: { rewindCapMs: u16 = 200; pveClientHits: bool = false };   // 04 §5.6 clamp; §8.6 PvE flag
  leaderboards: list<LeaderboardRef>;                           // §6.13
  client { name: LocString; art: AssetRef<Texture>; directorMarker: MapMarkerRef?; }
}
```

**Instance lifecycle.** The activity service (05 §1.12) and the host cell share one state machine. Its
state is the `ActivityState` value at `activity:<instanceId>` in KV `ACTIVITY`.

`Forming → Launching → Loading → Active ⇄ Wiping → Completed | Failed | Abandoned → Closing → Closed`

| State | Entered when | Behaviour |
|---|---|---|
| Forming | A premade launches from the director, or the matchmaker forms a roster (§6.13) | Matchmade rosters get a 30 s ready check. A decline or timeout returns the others to the front of the queue with their wait time kept |
| Launching | Everyone is ready | Each member's current cell validates that member's requirement, key item (consumed in one ledger transaction from the member's custody, idem `(instance, player, key)`), lockout eligibility (§6.8) and deserter tag (§6.10). The activity service then calls `CreateInstance` (04 §7) with the tier, modifiers and a salted seed |
| Loading | The instance is ready (≤ 2 s, AAA-SRV-11) | Players arrive by zone transition (04 §7). Encounters stay locked until the roster is in or 60 s pass |
| Active | The first player arrives | Encounters run (§6.7). Roster changes follow §6.10 |
| Wiping | An encounter's wipe condition holds | §6.7 wipe and reset, then back to Active |
| Completed, Failed, Abandoned | The last encounter completes or a match ends; the time limit expires, an objective fails or the PvE team loses; everyone has been gone for `abandonSecs` | Rewards (§6.8) and one idempotent result report (§6.13), then Closing |
| Closing | — | `closeGraceSecs` for loot and emotes, then players return to the zone and position saved at launch |

### 6.7 Encounters, checkpoints, wipes and revives

```
record EncounterDef @table("enc") {
  arena: VolumeRef; doors: list<DoorRef>;                       // doors lock (§9.6) while engaged
  start: variant { OnEnterVolume{ volume: VolumeRef }; OnInteract{ target: EntityRef };
                   OnAggro{ target: EntityRef }; AfterPrevious{ delaySecs: f32 } };
  phases: list<{ id: Name; enter: Condition; objectives: list<ObjectiveDef>;
                 spawns: list<SpawnWaveRef>; mechanics: list<AbilityRef | ScriptRef>;
                 checkpoint: bool = false; enrageSecs: Magnitude? }>;
  wipe: { when: enum { AllDown, AllOutsideArena, ObjectiveFailed, Custom }; custom: ScriptRef?; graceSecs: u8 = 3 };
  reset: { delaySecs: u8 = 10; respawn: RespawnPointRef };
  revive: RevivePolicy?;                                        // overrides the activity default
  loot: { table: LootTableRef; mode: enum { Personal, Group }; lockout: bool = true };   // §6.8
  contribution: ContributionWeightsRef?;                        // shared with §6.3
}
struct RevivePolicy {
  mode: variant { Free; Tokens{ perTeam: u8; regainPerPhase: u8 };
                  Timed{ baseSecs: u16; perDeathSecs: u16; maxSecs: u16 };
                  Waves{ intervalSecs: u16 }; None };           // None: no revive while engaged (Destiny darkness zone)
  incapSecs: u16 = 30; reviveHoldSecs: f32 = 3; selfRevives: u8 = 0; inCombat: bool = true;
}
```

- **Runtime.** An encounter is a data state machine like a quest: `Idle → Engaged(phase i) → Complete |
  Wiped`. Phases, spawns and mechanics run on the cell, and boss mechanics are server-only `AbilityDef`s
  (§7.1). Engaging locks the arena doors and tags every player in the arena `State.InEncounter`, which
  gates join-in-progress and vote-kicks (§6.10).
- **Wipe and reset.** When `wipe.when` has held for `graceSecs`:
  1. the encounter's enemies despawn, and the boss and its spawners return to their initial state;
  2. effects tagged `Scope.Encounter` are removed from players, and revive tokens reset;
  3. doors reopen, and after `delaySecs` downed players respawn at `reset.respawn`.

  Spent consumables are not refunded. The wipe count goes into the checkpoint and into telemetry (T12's
  per-encounter funnel).
- **Revive.** Death inside an activity runs the instance zone's `DeathPipelineDef` (§8.7) with the policy's
  incapacitation window. A revive is a `Hold` interaction (§9.6) on the downed player.
  - `Tokens` are counted per team and regained at phase checkpoints.
  - `Timed` lengthens the self-respawn timer with each death.
  - `Waves` respawns every dead player on the interval.
  - `None` forbids revives while engaged, so all players down is a wipe.
  - Out of combat, revives are always free.
- **Checkpoint contents.** A checkpoint is the `ActivityState` value, versioned by `(instance, seq)`.

| Checkpointed | Not checkpointed (reset or rebuilt) |
|---|---|
| Activity, tier, modifier set, seed, content pin, lockout period | NPC positions, health and brains |
| Roster: roles, team, join tick, AFK strikes, vote-kick state | Projectiles, cues, unminted reward tokens |
| Completed encounters; the engaged encounter's last `checkpoint: true` phase | Encounter progress past that phase |
| Objective counters outside encounters (flushed every 5 s) | Player positions: players respawn at the checkpoint's respawn point |
| Instance world flags (doors opened, bridges raised), revive tokens used, wipe counts | — |
| Lockout claims made so far; match score, round and clock (§6.11) | — |

  Writes happen on every encounter completion, every `checkpoint: true` phase, every score change
  (coalesced to ≤ 10 Hz) and every 5 s otherwise. Each is an async KV put of ≤ 4 KiB. PG snapshots
  follow on completion (05 §1.12).
- **Cell kill.** The orchestrator places a replacement cell, which rehydrates the zone from `WORLD` and
  `ACTIVITY` in ≤ 5 s (R05-P0-5, GP-14c). It restores completed encounters and instance flags, resets an
  engaged encounter to its last phase checkpoint (the Destiny behaviour) and respawns players at the
  checkpoint respawn point. Player AGs recover from their own checkpoint or replicant state as usual
  (04 §6.4). A PvP match restores score, round and clock, and resumes after a 10 s countdown.

### 6.8 Lockouts, resets and loot-once

```
record LockoutPolicyDef @table("lko") {
  scope: enum { PerEncounter, PerActivity };
  subject: enum { Character, Account, Legacy };                 // Legacy = account × shard (§5.2)
  reset: enum { Daily, Weekly, Season };                        // 05 §1.15 calendar events
  what: enum { Loot, Entry };                                   // Loot: re-runs allowed, no reward. Entry: no launch
  difficultyGroup: list<TagRef>;                                // tiers that share one lockout
  bindToInstance: bool = false;                                 // SWTOR/WoW save: re-enter only your saved instance
}
```

- **Period.** `periodId` is the shard calendar's period index (05 §1.15; weekly by default, Tuesday
  17:00 UTC) at the tick an encounter completes. Every cell applies `calendar.reset.*` at a tick boundary
  within 60 s. An instance that spans a reset keeps running, and its later completions fall in the new
  period.
- **Loot-once through a guard row.** Lockouts outlive the ledger's 72–96 h idempotency window (05 §3.3),
  so every claim names a **guard entity**, as 05 §3.3 requires. The ledger op `ClaimGuard(scope =
  "lockout", key, period)` inserts into `ledger_guard` only if the row is absent (05 §1.6). `key` is
  `(subject, activity, encounter or *, difficultyGroup)`.
  - At encounter completion the cell sends, per eligible player, one `LedgerTx{idem: (instance,
    encounter, player), reason: Faucet.Activity.<id>, ops: [ClaimGuard, Grant(reward tokens…)]}`.
  - The guard and the grant commit together or not at all. A retry, a replacement cell or a second
    instance in the same period gets `PRECONDITION_FAILED`, and the player sees "already rewarded this
    period". No path pays twice (GP-14b).
  - Pity counters (§2) advance through `ApplyProgression` under the same idem, so once per claim.
- **Eligibility.** At launch and on every join the cell reads each player's `LockoutView`, a per-character
  read over their guards that the activity service caches for 60 s. Ineligible players may still join a
  `Loot` lockout, are excluded from rolls and are marked in the roster UI. An `Entry` lockout refuses the
  launch.
- **Group loot.** In `Group` mode the cell first commits a `ClaimGuard`-only transaction for each eligible
  participant. It then rolls group loot among the players whose claims succeeded, and puts the tokens in
  a `Party` loot container (§9.6, need or greed).
- **Carried checkpoints (Destiny raids).** With `checkpointSource`, a new instance skips the encounters
  already completed this period: by the leader (`Leader`) or by every member (`Intersection`). The set is
  read from the guards themselves, so it can never disagree with rewards.
- **First clears.** `firstClearPerPeriod` rewards use the same guard with `encounter = *`.
- **Saved instances (SWTOR operations).** With `bindToInstance`, a subject's first claim in a period also
  records the instance ID in its guard row. Launching that activity and tier again in the period reopens
  the saved instance: the activity service recreates it from its last `ActivityState` snapshot (the
  instance itself was destroyed on idle, 04 §7), with completed encounters still cleared. A subject saved
  to one instance cannot join another instance of the same lockout key that period.

### 6.9 Difficulty, modifiers, level sync, bolster and scaling

```
struct DifficultyTierDef {
  tier: TagRef;                                                 // Difficulty.Story/Veteran/Master/Nightmare/Solo
  target: { level: u16?; power: u32? };
  enemyEffects: list<EffectRef>; playerEffects: list<EffectRef>;
  forcedModifiers: list<ActivityModifierRef>; modifierPicks: u8;
  revive: RevivePolicy?; matchmade: bool; lootTable: LootTableRef?; rewardScale: f32 = 1;
  unlock: Requirement?;                                         // e.g. Master needs a Veteran clear (§5.2)
}
record ActivityModifierDef @table("amod") {
  effects: list<{ filter: TagQuery; effect: EffectRef }>;       // e.g. Actor.Enemy.Elite → Shield.Arc
  scoreMult: f32 = 1; exclusiveGroup: Name?;
  client { name: LocString; icon: AssetRef<Texture>; }
}
struct SyncPolicy {
  level: enum { None, SyncDown, Bolster, Both } = None;
  power: { cap: u32?; floor: u32?; delta: CurveRef? };
  disableAbilitiesAbove: bool = false;
}
```

- **Tiers are data presets, not code.** The defaults are:
  - *Story*: solo-able with a companion.
  - *Veteran*: matchmade.
  - *Master* and *Nightmare*: premade, with modifiers and `Tokens` or `None` revives.

  Tier effects apply on instance entry and are removed on exit. They are tagged `Scope.Activity` and
  never persist.
- **Modifiers** apply through the `AreaVolume` modifier domain over the instance bounds (§1.2), so late
  spawns get them too. A weekly rotation reads the calendar. The picked set is part of the seed and the
  checkpoint, and `scoreMult` feeds leaderboard scores.
- **Level sync (down).** A `Sync` effect sets `Character.EffectiveLevel` to the target. It adds a `PreMul`
  modifier to every attribute tagged `Attr.Scalable`, with magnitude `syncScale(attrClass, level, target)`,
  an HXL curve derived from the level curve, marked `exempt` from stacking penalties. Gear, skill and
  legacy bonuses scale with the character, so build choices still matter. Abilities above the target
  stay usable unless `disableAbilitiesAbove` is set.
- **Bolster (up)** uses the same effect with `max(level, floor)`. Bolster never raises a character above
  the target and never touches an item's rolled stats, only the aggregate.
- **Power (Destiny).** `EffectivePower = min(Power, cap)` (§5.1). Outgoing and incoming damage each get a
  `PostMul` from `delta(EffectivePower − target)`. The default curve gives 0.63× outgoing and 1.6×
  incoming damage at −50, and no bonus above the target when a cap is set.
- **PvP normalization.** `MatchRulesDef.normalization` (§6.11) uses the same effects. `Stats` replaces
  gear-derived attributes with a per-class template (Crucible with power disabled), and `Bolster` is the
  SWTOR warzone form.
- **Scaling and solo.** `perPlayer` maps the number of players present to enemy HP and damage multipliers.
  It is an effect whose magnitude reads `ctx.rosterSize`, evaluated at encounter start and frozen for that
  encounter, so joining or leaving mid-fight cannot be exploited. `solo: true` enables the Solo tier:
  - a companion is auto-summoned (§7.3);
  - group-only mechanics are disabled by phase conditions on `Difficulty.Solo`;
  - `MultiUser` verbs (§9.6) accept one user.

### 6.10 Roster policy: backfill, join-in-progress, AFK, vote-kick and leavers

```
struct BackfillPolicy { enabled: bool = true; untilPct: u8 = 75; notWhileEngaged: bool = true; searchSecs: u16 = 90; }
struct AfkPolicy      { secs: u16 = 120; pvpSecs: u16 = 60; warnAtPct: u8 = 50; }
struct VoteKickPolicy { enabled: bool = true; quorum: enum { MajorityOfOthers, AllOthers } = MajorityOfOthers;
                        minVotes: u8 = 2; cooldownSecs: u16 = 300; maxPerRun: u8 = 2;
                        notWhileEngaged: bool = true; immunitySecs: u16 = 60; }
struct LeaverPolicy   { rejoinGraceSecs: u16 = 120; deserter: EffectRef?; deserterSecs: u32 = 900; }
```

- **Backfill.** A matchmade roster below its max asks the matchmaker for backfill for the missing roles
  (§6.13) while progress is below `untilPct`. Progress is completed encounters ÷ total, or match clock ÷
  limit. Backfill tickets sort ahead of new ones.
  - Joiners are admitted only between encounters, or at a PvP respawn wave.
  - With no one found in `searchSecs`, the roster continues short, and `perPlayer` scaling applies at the
    next encounter.
  - Backfillers get full completion rewards. Their lockout claims cover only the encounters they complete.
- **Join-in-progress.** Premade friends join through the party (`joinInProgress`) under the same rules:
  not while `State.InEncounter`, and subject to lockout eligibility.
- **AFK.** The cell counts each player's input-bearing ticks: movement over 1 m, ability activation,
  interaction, or damage or healing dealt. If none arrive for `secs` while an objective is active or the
  team is in combat, the player gets a warning cue at `warnAtPct`, then `State.AFK`.
  - AFK players earn no completion rewards and can be kicked without a vote.
  - In matchmade PvP they are removed after a further `pvpSecs` and get the deserter effect.
  - The signal is computed on the server. The client never reports it.
- **Vote-kick.** Any member can start a vote, and the target cannot vote. A vote passes by `quorum` with at
  least `minVotes`. It cannot start while `State.InEncounter` (PvE), within `immunitySecs` of the target
  joining, or more than `maxPerRun` times. Premade groups use leader kick instead. Kicks and their reasons
  go to the trust pipeline (05 §1.16), and 5 kicks of one player in 24 h flag the account for review. The
  kicked player keeps their lockout claims.
- **Leavers.** A disconnect holds the slot for `rejoinGraceSecs`, and the reconnect ticket returns the
  player to the instance. Leaving a matchmade activity early applies `deserter`, a `Wall` timed tag that
  blocks queueing: 15 min by default for PvP, and none for PvE.

### 6.11 PvP match rules (Crucible, warzones; G19)

```
record MatchRulesDef @table("mrd") {
  teams: list<{ id: TagRef; size: u8; spawns: list<RespawnPointRef> }>;   // 2 × 6, 2 × 8, FFA = 8 teams of 1
  objectives: list<variant {
    Elimination;                                                // last team standing wins the round
    Kills{ points: u16 = 1; assistPoints: u16 = 0 };
    Control{ zones: list<VolumeRef>; captureSecs: f32; tickSecs: f32; points: u16;
             contest: enum { AnyEnemyBlocks, Majority } };      // Destiny Control, SWTOR Alderaan
    Carry{ object: RecordTemplateRef; carrierEffects: list<EffectRef>; goals: list<VolumeRef>;
           points: u16; dropReturnSecs: f32 };                 // Huttball, capture the flag
    Escort{ path: SplineRef; speedByPlayers: CurveRef; checkpoints: list<f32> };
    Custom{ script: ScriptRef } }>;
  scoreLimit: u32; timeLimitSecs: u32;
  rounds: { count: u8 = 1; winsNeeded: u8 = 1; swapSides: bool = false; intermissionSecs: u8 = 5 };
  respawn: variant { Waves{ intervalSecs: u16 = 15 }; Individual{ baseSecs: u16; perDeathSecs: u16; maxSecs: u16 };
                     Lives{ n: u8 }; None };                    // None = elimination rounds
  spawnSafety: { protectSecs: f32 = 2; pick: enum { FarthestFromEnemies, TeamFixed }; minEnemyDist: f32 = 20 };
  mercy: { lead: u32?; leadRatio: f32?; afterSecs: u16 = 120 }?;
  overtime: { rule: enum { None, SuddenDeath, NextScore }; maxSecs: u16 = 120 };
  tieBreak: list<enum { Score, Kills, FirstToScore, Draw }> = [Score, FirstToScore, Draw];
  normalization: enum { Off, Stats, Bolster } = Off; friendlyFire: bool = false; abilities: TagQuery?;
  rating: RatingModelRef?;                                      // null = unrated
  medals: list<MedalRef>; rewards: { win: list<GrantDef>; loss: list<GrantDef>; perMedal: bool };
}
```

- **Match state machine.** `Warmup (until the roster is loaded, ≤ 60 s) → Countdown (10 s) → Round →
  RoundEnd (intermission) → … → Overtime → MatchEnd (20 s scoreboard) → Closing`. It is a data state
  machine on the cell. Its state is a replicated `MatchState{phase, round, clock, scores[teams],
  objectiveStates}` component, sent to every participant on change (≤ 10 Hz) and checkpointed (§6.7).
- **Scoring is server-authoritative and tick-stamped.** Every scoring event (a kill, a zone tick, a
  carry delivery, an escort checkpoint) is appended to the match's event log with its resolution tick.
  - Kills resolve through the normal damage pipeline with lag-compensated hits (04 §5.6, capped by
    `ActivityDef.netcode.rewindCapMs`). Credit goes to the instigator (a follower's owner, §7.3). Assists
    go to anyone who damaged the victim in the last 10 s.
  - The match ends at the tick `T_end` on which a limit is reached. Events with a resolution tick after
    `T_end` are discarded. Limits reached by two teams on the same tick go to `tieBreak`.
  - Final standings are a pure fold over the log, so a replay or an offline recomputation gives the same
    result (GP-14d).
- **Respawn.** Waves use one timer per team with a fixed offset between teams. `Individual` grows with
  each death. Spawn points are scored by distance to enemies that can see them, subject to
  `minEnemyDist`. Spawn protection (`State.Invulnerable.Spawn`) lasts until the player fires or
  `protectSecs` pass.
- **Mercy.** A team leading by `lead` points, or by `leadRatio`, after `afterSecs` wins at once. The losers
  get loss rewards and a normal rating update.
- **Result and rating.** At `MatchEnd` the cell sends `ReportMatchResult{matchId, resultHash, standings,
  perPlayer{team, score, medals, afk, deserted}}` to the activity service, idempotent by `matchId`.
  Rewards are ledger grants keyed `(matchId, player)`. The matchmaker applies the rating update once per
  `matchId` (05 §1.12).
  - `RatingModelDef` defaults to Weng-Lin with the Plackett-Luce model (as in OpenSkill: team ratings sum
    member μ and σ², and there is no patent encumbrance). Defaults: μ₀ = 25, σ₀ = 8.33, σ grows 0.1 per
    inactive week, 10 placement matches, and a visible rank from μ − 3σ.
  - Deserters and AFK-removed players take a loss. The remaining team's rating change is scaled by
    players present ÷ team size.
- **Integrity.** Friendly fire, ability filters and `CanHarm` (§9.4) come from the instance's zone rules.
  PvP never accepts client-reported hits (§8.6).

### 6.12 Opt-in PvP risk zones with extraction (R05-P2-22)

A `RiskZone` activity is a shared, capped PvPvE instance, like Destiny's Dark Zone or Star Citizen's
contested zones. Players may attack each other, and they must extract what they find.
- **Opt-in.** Entry is a gate verb with a confirmation prompt. Inside, the `ZoneRulesDef` sets `PvP.Open`.
  Leaving by any route except extraction forfeits carried finds.
- **At-risk loot.** Finds go into a per-character `Container.Unsecured`: a ledger container in the
  character's custody, tag-restricted, with 12 slots. Its contents cannot be traded, mailed or equipped
  until extracted.
- **Rogue.** Attacking a player who is not rogue grants `PvP.Rogue`, a crimewatch-style timed tag (§9.4)
  of 60 s that each hostile act extends. Rogues show on other players' maps (§9.8). Killing a rogue pays
  a bounty (`Faucet.Bounty.Rogue`) and grants no rogue status.
- **Extraction.** An `ExtractionPoint` interactable opens for 90 s on a timer. Extracting is a 10 s `Hold`
  (§9.6) that damage cancels. Success moves the `Container.Unsecured` contents into the inventory in one
  ledger transaction (reason `Transfer.Extraction`, idem `(instance, player, extraction seq)`).
- **Death.** The zone's `DeathPipelineDef` drops the unsecured contents into a lootable wreck (§8.7) and
  keeps equipped gear. Respawn is outside the zone.
- **Capacity.** 24 players and 40 AI per instance at 30 Hz by default. Population placement fills the
  instance with the fewest players in the same power band (05 §1.12).

### 6.13 Queues, matchmaking, group finder and leaderboards

05 §1.12 specifies the services, their algorithm and their capacity. 06 owns the records they read and the
reports that cells send.

```
record QueueDef @table("que") {
  activities: list<{ activity: ActivityRef; tier: TagRef; weight: f32 }>;   // a playlist
  pool: enum { PvE, PvP }; partySizes: list<u8>;
  roles: { required: bool; fillBonus: list<GrantDef> };         // bonus offered for the scarce role
  rating: RatingModelRef?;
  latency: { initialMs: u16 = 60; stepMs: u16 = 30; stepSecs: u16 = 30; maxMs: u16 = 150 };
  widen: { initialSigma: f32 = 1.0; perSec: f32 = 0.05; maxSigma: f32 = 4; winProb: { initial: f32 = 0.1; max: f32 = 0.25 } };
  premades: enum { Mix, PreferPremadeOpponents } = PreferPremadeOpponents;
  maxWaitSecs: u16 = 900; readyCheckSecs: u8 = 30;
}
record LeaderboardDef @table("lbd") {
  metric: variant { Score; ClearTimeTicks; Rating; Counter{ event: EventTypeRef; filter: TagQuery } };
  scope: { activity: ActivityRef?; tier: TagRef?; window: CalendarWindowRef };   // weekly, season, all-time
  entrant: enum { Player, Team, Guild }; keep: enum { Best, Latest, Sum };
  partitions: list<enum { Shard, Friends, Guild, Class }>; topN: u16 = 1000;
}
struct GroupListing { kind: enum { Activity, Fleet } = Activity;   // Fleet: an advert (05 §1.12.1)
                      activity: ActivityRef?; goal: TagRef?;                     // goal and joinAcl: fleets only
                      joinAcl: { scope: enum { Public, Corp, Alliance, Standing, List }; minStanding: f32?;
                                 chars: list<CharacterId>? }?;
                      tier: TagRef?; wanted: list<{ role: TagRef; n: u8 }>;      // tier: activities only
                      requirement: Requirement; note: string;   // 140 chars, sanitized (05 §1.10)
                      voice: bool; lang: LangTag; visibility: enum { Public, Guild, Friends }; }
```

- **Tickets.** A party leader queues. Each member's cell validates that member's entry requirement,
  lockout eligibility and deserter tag. The leader's cell then sends the matchmaker a ticket
  `{party, members{char, rolesMask, μ, σ, power}, rttMs, lang, deviceClass, createdAt}`. Parties are never
  split. Each member picks one or more roles.
- **Formation rules.**
  - Role minimums are hard and never widen.
  - The rating window and the latency band widen with wait time, as `QueueDef` sets.
  - PvP teams are split so the predicted win probability is 0.5 ± 0.1, widening to ± 0.25.
- **Group finder.** Players publish a `GroupListing`, and others filter by activity, tier, role and
  requirement, then apply. The leader accepts, which forms an ordinary party that launches as a premade.
  The server evaluates the requirement ("Veteran clear", "power ≥ 1,800") on each application, so a
  listing can demand facts a player cannot fake.
- **Leaderboards** take only server results: `ReportActivityResult` and `ReportMatchResult` carry the
  metric values with the `resultHash`. A clear time counts zone ticks from the first encounter's start to
  completion, so time dilation cannot shorten it. Instances with a GM intervention or a failed audit are
  excluded.
- **Director.** The client's activity director (08 §1.7) lists activities, tiers, modifiers, per-character
  lockout state and queue estimates, from the records and a `LockoutView` read.

## 7. AI

### 7.1 Brains, perception and navigation

- **Brain (R09-A13).** Each NPC's `Brain` combines a **behavior tree** (compiled from the AI graph
  to a flat node array, with running-node resumption and event interrupts), **utility selectors**
  (IAUS response curves, multiplied with a compensation factor), a typed blackboard per record template,
  and an optional **HTN** planner for squad tasks (Phase 4). NPCs use the player kernel:
  server-only `AbilityDef`s and the same effects and damage pipeline, so a boss mechanic is
  authored like a player ability.
- **Perception** uses the player sensor model (§8.5): sight cones with time-sliced LOS rays, noise
  events, damage and space contacts. It feeds decaying target memory and a **threat table**.
- **Navigation.** Recast/Detour tiled navmesh **per physics grid** (ADR-005). Authored areas are
  baked into object containers. Procedural terrain generates tiles at runtime near activity, on
  the pathfinding pool. **Ship interiors** are baked in ship-local space, and agent positions are
  grid-local, so a moving or rotating ship never invalidates paths. `GridLink` off-mesh
  connections (airlocks, ramps, elevators) trigger `Reparent()`. Door state (§9.6) sets area costs
  and disables links through locked doors. DetourCrowd is capped per grid.
- **Space AI.** Steering runs on top of the movement models (§8.1): seek, arrive, orbit and
  keep-at-range, align, warp-out. Formations are slots in the leader's frame. A **fleet/squad
  coordinator** entity uses utility scoring to issue primary targets, remote-repair assignments,
  EWAR distribution and retreat.

### 7.2 Spawners and AI LOD

- **Spawners.** A `SpawnerDef` has a volume, population entries (record template, weight, level, group
  size, conditions on time, world flags and economy weights), respawn timers and a max-alive count.
  Lairs are destructible spawners with waves. `PopulationDef` sets regional densities.

**AI LOD.** The tier is re-evaluated every second from observer distance, combat state and
relevance.

| Tier | Decisions | Perception | Movement |
|---|---|---|---|
| T0 Active | 10–20 Hz | Full | Crowd + nav |
| T1 Reduced | 1–2 Hz | Coarse, no LOS | Nav only |
| T2 Dormant | Event-driven | Wakes on stimulus | Frozen or spline |
| T3 Virtual | — | — | A population row, materialized when a player approaches (R09-A13) |

- **Execution.** Brains run as parallel, time-sliced jobs under a per-zone budget. They emit
  commands that apply in the sync phase instead of writing to other entities, and each NPC has a
  seeded RNG, so replays reproduce exactly.
- **Ecosystem simulation (Phase 5).** `EcosystemDef` predator, prey and herd dynamics run on the T3
  layer. This revisits SWG's cut "dynamic world" within budgets (R02 §8.7).

### 7.3 Owned NPCs: companions, pets, hirelings, crew and drones (G11; R03-P1-7, R02-Q17)

One framework covers SWTOR companions, SWG creature-handler pets and hirelings, Star Citizen NPC
crew, and EVE drones and fighters. An owned NPC is an ordinary NPC (§7.1 brain, kernel abilities,
damage model) plus an owner, a command channel and durable state backed by a ledger item.

```
record FollowerDef @table("flw") {
  kind: enum { Companion, Pet, Hireling, Crew, Drone, Fighter };
  template: RecordTemplateRef;        // attributes, abilities, damage model, movement model (§8.1)
  brain: BrainRef; commands: CommandSetRef;
  stances: list<StanceDef>;           // role presets (Tank/Healer/DPS, Passive/Defensive/Aggressive) = modifier sets
  capacity: map<AttrRef, f64>;        // cost against owner caps: Follower.Max.<kind>, PetControl, DroneBandwidth
  leash: Magnitude;                   // ground default 40 m; ≤ the owner class's interest margin (cook check)
  controlRange: Magnitude?; scoopRange: Magnitude?;                 // drones: 20–60 km, 2.5 km
  summon: { castSecs: Magnitude; blockedBy: TagSet; cooldownSecs: Magnitude; autoResummon: bool };
  gear: ContainerSpec?;               // slots on the control device (companions, hirelings)
  progression: ProgressionGraphRef?;  // levels, influence ranks, learned commands, crew skills
  influence: { xp: XpTypeRef; ranks: LevelCurveRef }?;
  availability: Condition?;           // story facts can make a companion leave (R03-P1-7)
  onDefeat: enum { Incapacitate, ReturnToDevice, DestroyItem };
  zones: TagQuery;                    // ZoneRulesDef tags where this kind may be out (no pets in space)
  client { portrait: AssetRef<Texture>; voiceSet: VoiceSetRef; barks: BarkSetRef; }
}
```

**Backing item and summoning.** Companions, pets, hirelings and crew are represented by a
`ControlDeviceDef` item, the same device type mounts use (§9.5). Drones and fighters are represented by
their own item. *Call* is a ServerOnly ability (default 3 s cast, blocked by `State.InCombat` for
companions) that spawns the follower at a free formation slot. *Store* despawns it and returns it to
the device. On summon the cell checks Σ `capacity` against the owner's cap attributes and the kind's
`Follower.Max.<kind>` count, `availability`, `zones`, and that the follower is not `State.OnMission`.

**Authority, handoff and recovery.**
- **Membership.** A summoned follower carries the pair `(FollowerOf, owner)`. This is an
  **authority attachment**: the follower is a member of the owner's AG with no fence row of its own
  (04 §6.1), not a physical child. It keeps its own `InFrame` and moves freely.
- **Handoff and transfer.** The owner's `HandoffOffer` carries every follower in the same message
  (04 §6.3), so followers change cell on the same tick as their owner. A zone transfer (04 §7) does
  the same. The destination respawns followers at formation slots around the arrival point. A kind
  that the destination's `zones` rule forbids is auto-stored on departure and re-summoned on return
  if `autoResummon` is set.
- **Recovery.** Crash recovery restores followers from the owner's checkpoint `(epoch, seq)` (05
  §1.13), or, in world zones from Phase 4, from the owner's replicant state (04 §6.4). A follower
  is never newer or older than its owner.
- **Leash.** `leash` never exceeds the owner class's interest margin M (04 §6.2, a cook-time check),
  so a neighbour cell always holds a ghost of any follower near its boundary and routes damage to it
  as `ApplyDamage` to the owner's AG. Beyond the leash, or after 3 s without a path, the follower
  teleports to the nearest navmesh point by the owner at the owner's next safe moment.
- **Boarding.** When the owner boards (`Fence.Join`), followers within 20 m reparent with them through
  the same `GridLink`. The rest teleport in.

**Persistence: three stores, no new service.**

| State | Store | Written |
|---|---|---|
| Identity, rolled stats, name, gear | Ledger: the device or drone item, with gear items located `(deviceItem, flag = slot)` in the owner's custody | Synchronously: `Mint` on acquisition, `MoveItem` to equip, `Modify` to rename |
| Level, influence, learned commands, crew-skill levels | Character service progression, keyed by the device item ID (05 §1.5) | Async, idempotent `ApplyProgression`, like the owner's XP |
| HP, `persist` effects, stance, current command, summoned flag, pose | The owner's AG checkpoint | Write-behind with the owner |
| Crew missions | Industry job + durable timer (05 §1.8) | Dispatch and deliver ledger transactions |

Followers that carry progression are `bind: Character`. Tradable followers (drones, bio-engineered pet
deeds) have immutable stats in their item and no mutable progression. Gear equips through the
player's own `MoveItem` intent path (§2), and gear `stateEffects` apply to the follower through the
kernel. A cell kill can therefore roll back pose or a few seconds of XP, but it can never duplicate
gear.

**Commands.** The owner holds a **`Followers` input channel**, granted by owning summoned followers
and routed like a seat's channels (§8.4):

```
FollowerCommand { selector: All | Group(u8) | One(handle);
                  verb: Follow | Stay | Guard | Assist | Attack | Passive | Defensive | Aggressive
                        | UseAbility(ability) | Return | Store | Mine | Formation(slot);
                  target: EntityHandle?; pos: WorldPos?; }
```

- **Validation.** Intents are schema'd and rate-limited to 10/s. The cell checks the sender against
  the owner, or against a delegate ACL (`Perm.Follower.Command`: EVE drone assist to a fleet member, a
  captain ordering NPC crew).
- **Delivery.** The command writes `cmd.verb`, `cmd.target`, `cmd.pos` and `cmd.seq` into the
  blackboard. Every follower brain's root is the built-in `CommandSelector` node, which preempts the
  running subtree when `cmd.seq` changes. Response is the next AI decision after the arrival tick.
- **Chat phrases (SWG).** Custom phrases such as "Fluffy, attack" are matched by the owner's client
  against its own outgoing chat and sent as `FollowerCommand` intents. The server never parses chat.
- **Combat attribution.** Follower damage packets set `instigator = owner` (§8.7). Hostile commands
  pass `CanHarm(owner, target)`, and threat, crimewatch, killmails and `Kill` quest credit go to the
  owner.

**Brains and budget.** Followers count against the zone's AI budget (§7.2, GP-3) and never drop below
T1 while summoned. They run at T0 when the owner is in combat or has commanded them in the last 5 s.
Otherwise they escort at T1 (2 Hz decisions, crowd steering to formation slots every tick, coarse
perception). Fleet-battle zones move drones and fighters as `CommandKinematic` entities (§8.1) with a
three-state brain (Idle, Engage, Return).

**Kinds.**
- **Companions (SWTOR).** A companion has a gear container, stances as role presets, and influence as
  typed XP `Influence.<companion>`. Influence rises from dialogue `influenceDeltas`, gifts (consumed
  through the ledger with `Sink.Gift`) and crew missions. Influence ranks grant stat effects and gate
  companion conversations, which are `DialogueDef`s whose conditions read the rank. `availability`
  facts let the story remove a companion. `onDefeat: Incapacitate`: the owner can revive it, and it
  auto-revives at 50 % health when combat ends.
- **Pets and taming (SWG creature handler).** `Tame` is a channel ability whose chance is an HXL
  formula over creature level and skill. On success the cell mints a bound `ControlDevice` with the
  creature's template and seeded, audited rolled stats. The wild entity converts in place, and the
  transient NPC becomes a follower without despawning. Trained commands are progression nodes.
  `PetControl` capacity limits how many pets are out. `onDefeat: ReturnToDevice` with a recovery
  cooldown.
- **Hirelings and crew (SC).** A hireling is bought at a `HireableDef` vendor terminal; its wages are a
  durable-timer sink. Crew occupy seats and stations: the cell routes a seat's channels to the crew
  brain (§8.4), so AI crew press the same inputs as players.
- **Drones (EVE).** Drones are items in the ship's `DroneBay` container, with ship `DroneBandwidth`
  and character `MaxActiveDrones` (5 by default) and `DroneControlRange` as caps.
  - *Launch* is a ServerOnly ability: 1 s per drone, 5 per activation. It does not touch the ledger.
    The item stays in the bay and is locked as `Launched` in the ship's checkpoint, so a crash returns
    it to the bay.
  - *Recall* needs the drone within `scoopRange`. Drones outside `controlRange` idle.
  - *Abandon.* If the owner leaves the grid, or logs off, with drones out for more than 10 s, each
    drone is abandoned: one ledger transaction, `CreateAg` + `MoveItem(drone → space)`, like jettisoned
    cargo (04 §6.1). It becomes a persistent root that anyone may scoop, under a 2 h lifetime policy.
  - A destroyed drone is a ledger `Destroy`.
  - Skills modify launched drones through the `Followers` modifier domain (§1.2).
- **Fighters (EVE carriers, SC).** Fighters are persistent member AGs in a carrier hangar (04 §6.1)
  that obey the same command channel.

**Crew-skill missions (SWTOR).**
- **Record.** `CrewMissionDef{skill: CrewSkillRef, duration: HxlExpr, cost: CurrencyAmount,
  rewardTable: LootTableRef, critChance: HxlExpr}`. Crit chance reads influence rank and skill level.
- **Dispatch.** The cell validates the companion (available, not summoned, a free `CrewMissionSlots`
  slot, 5 by default). It then **rolls the outcome at dispatch** with a seeded, audited stream,
  charges the cost in `job:<id>:start`, and registers an Industry job (05 §1.8). The job carries the
  sealed outcome, and the duration is recomputed in Go HXL.
- **Progress.** The companion is tagged `State.OnMission`. The job progresses offline.
  `job:<id>:deliver` mints the rolled outputs, so Go never rolls RNG and outcomes stay audited.
  Cancelling refunds 50 % and discards the outcome.

**Authoring.** T23 authors follower brains, command sets, stances and leash or control-range
previews, and checks drone bandwidth budgets. T13 authors per-choice influence deltas (the simulator
shows "approves" and "disapproves") and companion conversations gated by rank. T09 authors
crew-mission tables with its loot simulator. Every record is also editable in T08.

## 8. Vehicles, flight and combat

### 8.1 Movement models (R09-A1, R09-G3)

Each record template picks one model. The gameplay controllers sit on 02-engine-runtime's physics:
- `CharacterVirtual`: on foot, grid-local, with a radial or grid gravity up vector (02 §7.1).
- `EVA`: a suited character below 0.05 g, predicted in 6-DoF with suit thrusters (§8.2a). A character
  switches between `EVA` and `CharacterVirtual` at grid transfers and gravity thresholds.
- `NewtonianFBW`: 6-DoF ships.
- `CommandKinematic`: EVE "ball" movement in 2 Hz fleet-battle zones (01 §3.4, 04 §3.4); commands replicate, not transforms.
- `Wheeled`, `Hover` and `Mount`: ground vehicles and ridden creatures (§8.1a). Wheeled and tracked
  vehicles run Jolt's `VehicleConstraint` with engine, gearbox and differential records. Hover vehicles run
  a ray-suspension controller with height and tilt limits. Mounts run a `CharacterVirtual` mount mode with
  a rider attach and their own animation sets. The driver or rider holds the `Drive` input channel.
- `Travel`: warp and quantum travel.

**Phases (01 §2.6 M03).** On-foot `CharacterVirtual` ships in Phase 1 (WP-1.16). `Wheeled`, `Hover`,
`Mount` and the `Drive` channel ship in Phase 2: WP-2.2 integrates the runtime (02 §7.1) and WP-2.9 the
gameplay, with GP-4d as the acceptance. Tracked and motorcycle drivetrains, mounted combat and vehicle
turrets follow in Phase 3.

### 8.1a Ground vehicles and mounts (M03; SWG, DST, SC, TOR)

SWG's speeder bikes, swoops and creature mounts, Star Citizen's rovers and hoverbikes, SWTOR's speeders
and Destiny's Sparrows share one contract. A `VehicleDef` on a record template picks one of three
controller models. All three share the seats, the `Drive` channel, the prediction contract, validation, and
the call and store rules below. Until Phase 5 a vehicle has exactly one dynamic body. Articulated vehicles
(trailers, legged walkers) come in Phase 5, and a vehicle large enough to walk inside is a grid, like a
ship (§8.2a), not a set of seats.

```
record VehicleDef @table("veh") {
  model: enum { Wheeled, Hover, Mount };
  mass: Magnitude;                             // kg: the chassis body (Wheeled, Hover) or the mover (Mount)
  collision: CollisionProfileRef;              // object layer Vehicle; Character for mounts
  seats: list<SeatRef>;                        // seat 0 grants Drive (§8.4); others: passenger, TurretGroup[n]
  maxSpeed: Magnitude; reverseSpeed: Magnitude = 8;             // m/s, enforced by the governor
  boost: { mult: f64 = 1.5; speed: Magnitude; energy: Magnitude = 100; drainPerSec: Magnitude = 25;
           regenPerSec: Magnitude = 10; cooldownSecs: Magnitude = 2 }?;
  fuel: { capacity: Magnitude; perKm: Magnitude }?;             // SC rovers; SWG vehicles have none
  impact: { thresholdMps: Magnitude = 8; scale: f64; riderShare: f64 = 0.25 };
  call: { castSecs: Magnitude = 3; indoors: bool = false; zones: TagQuery; idleStoreSecs: Magnitude = 600 };
  wheeled: WheeledDef?; hover: HoverDef?; mount: MountDef?;    // exactly one, matching `model` (cook check)
  client { anims: AnimGraphRef; hud: UiSurfaceRef; engineAudio: AudioEventRef; }
}
```

**Envelope and stepping.**
- **Envelope.** Every ground vehicle stays inside 02 §5.8a's ground-vehicle workload row: `maxSpeed` and
  `boost.speed` ≤ 100 m/s, and peak flat-ground acceleration ≤ 8 m/s². T21's envelope preview measures the
  acceleration from the cooked drivetrain, and T28's `phys.vehicle.envelope` rule fails the cook otherwise.
  Anything faster is a ship.
- **Substeps.** A bubble that holds a `Vehicle`-layer body steps physics at 60 Hz, as open space does
  (04 §3.3): `60 Hz / tick rate` collision steps per tick, so 3 at 20 Hz, 2 in a 30 Hz activity and 1 at
  60 Hz. A speeder at 100 m/s therefore moves ≤ 1.7 m per step, and a wheel cast never skips a tile edge.
  The owner's predictor always uses the same count, because its own chassis is in that bubble. Inputs hold
  across the steps of a tick.
- **Listeners.** The wheeled and hover controllers are Jolt `PhysicsStepListener`s (a `VehicleConstraint`
  already is one), so they run on every collision step and touch only their own body. Mounts move in the
  character mover at tick rate, as characters do.
- **Grids.** A vehicle in a ship's hangar grid steps in that grid (W02), and drives out through the hangar's
  `GridVolume` transfer (02 §5.4) with its velocity re-expressed. It is predicted through the transfer, as
  EVA characters are (§8.2a). This comes in Phase 3, with hangars (02 §8.1).

#### Wheeled: `VehicleConstraint`

```
record WheeledDef {
  drivetrain: enum { Wheeled, Tracked, Motorcycle } = Wheeled;  // Tracked and Motorcycle: Phase 3
  engine: { maxTorque: Magnitude; minRpm: f64 = 1000; maxRpm: f64 = 6000;
            torqueCurve: CurveRef;               // normalized torque over normalized rpm, ≤ 8 points
            inertia: f64 = 0.5; angularDamping: f64 = 0.2 };
  gearbox: { mode: enum { Auto, Manual } = Auto; ratios: list<f64>(1..8); reverse: list<f64>(1..2);
             switchSecs: f64 = 0.5; clutchReleaseSecs: f64 = 0.3; switchLatencySecs: f64 = 0.5;
             shiftUpRpm: f64 = 4000; shiftDownRpm: f64 = 2000; clutchStrength: f64 = 10 };
  differentials: list<{ left: u8; right: u8; ratio: f64 = 3.42; leftRightSplit: f64 = 0.5;
                        limitedSlip: f64 = 1.4; engineTorqueShare: f64 }>(1..4);   // shares sum to 1
  wheels: list<{ port: PortId; radius: Magnitude; width: Magnitude;
                 suspension: { minLen: Magnitude; maxLen: Magnitude; preload: Magnitude = 0;
                               frequencyHz: f64 = 1.5; damping: f64 = 0.5 };
                 maxSteerDeg: f64 = 0; maxBrakeTorque: f64 = 1500; maxHandbrakeTorque: f64 = 4000;
                 longFriction: CurveRef; latFriction: CurveRef;  // over slip ratio / slip angle, ≤ 8 points
                 inertia: f64 = 0.9 }>(2..8);
  antiRoll: list<{ left: u8; right: u8; stiffness: f64 = 1000 }>;
  tester: enum { Ray, Sphere, Cylinder } = Cylinder;     // wheel contact casts
  maxSlopeDeg: f64 = 60; maxPitchRollDeg: f64 = 60;       // Jolt's mMaxSlopeAngle and mMaxPitchRollAngle
  steerCurve: CurveRef;                        // steer scale over speed, e.g. 1.0 at 0 → 0.35 at maxSpeed
  tractionAssist: bool = true;
}
```

The fields map one to one onto Jolt's `VehicleConstraintSettings` and `WheeledVehicleControllerSettings`
(`VehicleEngineSettings`, `VehicleTransmissionSettings`, `VehicleDifferentialSettings`, `WheelSettingsWV`,
`VehicleAntiRollBar`), and onto `TrackedVehicleControllerSettings` and `MotorcycleControllerSettings` in
Phase 3. Curves cook to Jolt `LinearCurve`s. The constraint has **one body**, the chassis. Wheels are casts
plus per-wheel state inside the constraint, not bodies, so the predictor steps one body and one constraint.
Each tick:
1. **Input.** `forward = throttle/255`, `brake = brake/255`, `right = steer/127 × steerCurve(v)` and
   `handbrake ∈ {0, 1}`. Holding brake for 0.3 s below 1 m/s selects reverse (`forward = −brake`). A manual
   gearbox shifts on `ShiftUp` and `ShiftDown`.
2. **Governor.** `forward ← min(forward, max(0, (vCap − v_fwd)/(2 m/s)))`. `vCap` is `maxSpeed`, or
   `boost.speed` while boosting, or `reverseSpeed` in reverse.
3. **Boost.** While boost is active, `engine.maxTorque × boost.mult` is written at the tick boundary, as a
   discrete state.
4. **Traction assist.** When a driven wheel's slip ratio `|ω·r − v|/max(|v|, 1 m/s)`, latched from the
   previous step, exceeds 0.25, `forward` is halved for the tick.
5. **Jolt.** `WheeledVehicleController::SetDriverInput(forward, right, brake, handbrake)` (a tracked
   vehicle gets left and right track ratios from `steer`). The constraint then steps with the body on each
   collision step: engine rpm, clutch, automatic shifting, limited-slip differentials, suspension springs,
   friction curves and anti-roll bars. Friction comes from the physical material of the sub-shape hit
   (02 §7.1).

#### Hover: ray suspension

```
record HoverDef {
  pads: list<PortId>(2..8);                   // ray origins, in cooked port order
  hoverHeight: Magnitude = 1.0; trimRange: { min: Magnitude = 0.5; max: Magnitude = 2.0 };
  rayLength: Magnitude = 3.0;                 // no lift beyond it; cook check: ≥ trimRange.max + 0.5 m
  frequencyHz: f64 = 2.5; dampingRatio: f64 = 0.7; maxPadForceG: f64 = 3.0;
  thrust: Magnitude; reverseThrust: Magnitude; brakeDecel: Magnitude = 12;
  lateralGrip: f64 = 4.0; driftGrip: f64 = 0.8;            // 1/s; Handbrake selects driftGrip
  turnRate: CurveRef;                         // deg/s over speed, e.g. 120 at 0 → 45 at maxSpeed
  yawResponseHz: f64 = 3; bankDeg: f64 = 20;
  maxTiltDeg: f64 = 25; tiltSoftDeg: f64 = 15; rightingHz: f64 = 2;
  maxSlopeDeg: f64 = 35; overWater: bool = true; dragArea: f64 = 0.6;     // m², C_d·A
}
```

The controller runs on each collision step in f64 with `hmath`, and sums over pads in pad order:
1. **Rays.** Each pad casts `rayLength` along −up (radial on planets, grid up indoors) against the Terrain,
   Static and ShipHull layers. With `overWater`, it also tests the sea plane of the zone's `WaterBodyDef`:
   flat in Phase 2, the Gerstner set from Phase 3 (02 §7.1). The nearest hit wins. Ties are broken by
   `TileKey` or entity ID, then sub-shape ID, and never by Jolt `BodyID`, because the predictor's body IDs
   differ from the cell's.
2. **Lift.** The target height is `h* = clamp(hoverHeight + trim/127 × (trimRange.max − trimRange.min)/2,
   trimRange.min, trimRange.max)`. Each pad that hits applies `F_i = clamp(k(h* − h_i) − c·v_i, 0, F_max)`,
   with `k = (m/N)(2πf)²`, `c = 2ζ(m/N)(2πf)` (f = `frequencyHz`, ζ = `dampingRatio`), `F_max = maxPadForceG·m·g/N` and `v_i` the pad's velocity
   along up. A 400 kg landspeeder on 4 pads gets `k` ≈ 24.7 kN/m and sags 4 cm at rest. A pad that misses
   gives 0, so a hover vehicle cannot climb beyond `rayLength`, and falls off cliffs.
3. **Drive.** Thrust is `throttle/255 × thrust × (boost ? boost.mult : 1)`, along the forward axis projected
   on the mean hit normal, with `throttle/255` first capped by the wheeled governor (step 2 above). Brake decelerates at `brakeDecel` to 0, then
   reverses under `reverseThrust`. On a slope steeper than `maxSlopeDeg` the uphill component of thrust is
   removed, so a speeder slides back down a cliff face.
4. **Grip and yaw.** The lateral force is `−m·grip·v_lat`. A yaw torque drives the yaw rate toward
   `steer/127 × turnRate(v)`, with a proportional gain set by `yawResponseHz`.
5. **Tilt.** A PD torque at `rightingHz` pulls the vehicle's up toward the mean hit normal, rolled by
   `−bankDeg × steer/127 × min(1, v/(20 m/s))`. Past `tiltSoftDeg` the gain rises 4×, and any torque
   component that would push tilt past `maxTiltDeg` is removed.
6. **Drag.** `−½ρ·dragArea·|v − w|(v − w)`, with `w` from §8.2 rule 8's `ambientWind` schedule and ρ
   from the body's atmosphere record.
7. **Apply.** The summed force and torque are rounded once to Jolt's float input and applied to the chassis.

#### Mount: `CharacterVirtual` mount mode

```
record MountDef {
  capsule: { radius: Magnitude = 0.7; height: Magnitude = 2.4 };
  frontProbe: { radius: Magnitude = 0.5; offset: Magnitude = 1.2 };  // a chest sphere ahead of the capsule
  gaits: list<{ name: TagRef; speed: Magnitude; accel: Magnitude; turnRateDeg: f64;
                staminaPerSec: Magnitude = 0 }>(2..5);  // speed and turn rate cooked from the clips (T15)
  stamina: { max: Magnitude = 100; regenPerSec: Magnitude = 10; resumePct: u8 = 25 };
  stepHeight: Magnitude = 0.5; maxSlopeDeg: f64 = 40; maxWadeDepth: Magnitude = 1.2;
  jump: { track: RootMotionTrackRef; minGait: u8 = 1 }?;
  saddle: SocketRef; dismountPoints: list<SocketRef>(1..3);        // left, right, rear
  mountedAbilities: TagQuery = Ability.AllowMounted;
  client { mountAnims: AnimGraphRef; riderAnims: AnimGraphRef;
           reins: SocketRef[2]; stirrups: SocketRef[2]; }
}
```

- **Mover.** A mount is a `CharacterVirtual` with its own capsule. It runs through the shared mover (04 §5.2)
  with `ExtendedUpdate`, `stepHeight`, `maxSlopeDeg` and the current up vector, so RT-03's character hash
  covers it. `throttle` sets a target speed on the gait table, up to the second-fastest gait. `Sprint`
  selects the fastest gait while stamina lasts. `brake` slows the mount, and below 0.5 m/s backs it up at
  1.5 m/s. Speed changes at the gait's `accel`, interpolated between gaits, and heading turns at
  `steer/127 × turnRateDeg`, interpolated the same way. Mounts do not strafe.
- **Front probe.** Each tick a sphere cast starts `offset` ahead of the capsule and runs `speed·dt + 0.2 m`
  along the heading. A hit clamps speed so the long body stops before the wall. Water deeper than
  `maxWadeDepth` caps speed at the slowest gait and blocks deeper travel.
- **Stamina** drains at the gait's `staminaPerSec`, and is predicted state, like fuel. At 0 the mount drops
  one gait until stamina is back to `resumePct`.
- **Animation-driven locomotion.** At cook time T15 extracts each gait clip's mean root velocity and turn
  rate into `gaits[]`, so the authored animation sets the gait speeds. The simulation never samples ozz for
  movement; cells sample only hitbox joints (02 §7.2). The client blends a `BlendSpace2D` over speed and turn
  rate across the gait clips. It plays each clip at `speed/clipSpeed`, clamped to [0.85, 1.15], uses stride
  warping for the rest, and runs foot IK: one `TwoBone` chain per leg (four on a quadruped), with the
  pelvis pitched to the ground normal. **Jump** is an ASM ability with `SetMovementOverride{RootMotion, track}`. Both sides play
  the cooked `RootMotionTrack` (per-tick deltas at 1/1024 m) and keep horizontal speed.
- **Rider attach.** Mounting is a predicted ASM ability (`SetMovementOverride{Attach, saddle}`), allowed at
  ≤ 2 m/s. The rider's own `CharacterVirtual` is suspended, and its transform follows the saddle socket at
  the mount's bind pose, so neither side needs the animated pose. The rider plays `riderAnims`: a seated pose
  per gait, an additive bob synced to the mount's gait phase, and hand and foot IK to the reins and
  stirrups. The rider's `HitboxSetDef` switches to its mounted variant. **Dismount** shape-casts the standing
  capsule at each of `dismountPoints` in order and takes the first clear one. If none is clear, it is denied.
- **Creature mounts** are pets (§7.3) whose record template carries a `VehicleDef` with `model: Mount`. The
  same control device calls, stores and rides them (§9.5).

#### Seats, combat and impacts

- **Seats.** Seat 0 of every vehicle and the saddle of every mount grant **`Drive`** (§8.4). Other seats
  grant a passenger pose or, from Phase 3, `TurretGroup[n]`. Passengers attach to seat sockets and have no
  body.
- **Mounted combat.** `State.Mounted` blocks every ability outside `mountedAbilities`.
  `ZoneRulesDef.mountedCombat` is `Deny` (SWG, and the only mode in Phase 2), `Tagged` (the default from
  Phase 3) or `All`. A single `DamagePacket` above `ZoneRulesDef.dismountOnHitPct` (default 20 %) of the
  rider's maximum health forces a dismount.
- **Impacts** are resolved on the cell from contact events, never predicted. The vehicle takes
  `scale × max(0, |Δv_n| − thresholdMps)²`, and each occupant takes `riderShare` of it. Hitting a character
  applies a server-only `VehicleImpact` effect: knockdown, with damage gated by §9.4's PvP flags. At zero
  health the vehicle's `DeathPipelineDef` ejects its occupants at the vehicle's velocity, capped at 10 m/s.

#### The `Drive` input channel

`Drive` travels in 04 §5.2's input record in place of `Flight`'s axes, since a seat grants one of the two,
never both:

```
Drive { steer: s8; throttle: u8; brake: u8; trim: s8;
        buttons: u16 { Boost, Handbrake, ShiftUp, ShiftDown, Jump, Sprint, Horn, Lights, Dismount } }
```

- **Size.** 6 bytes before delta coding, smaller than `Flight`. Look yaw and pitch still travel, for the
  camera and mouse steering.
- **Device shaping.** Device curves, keyboard ramps (0 → 255 over 0.25 s) and mouse steering (steer toward
  the camera's yaw) run on the client before quantization, never inside the predicted step. `trim` is used
  only by hover vehicles, `ShiftUp` and `ShiftDown` only by manual gearboxes, and `Sprint` and `Jump` only
  by mounts.
- The client pushes 08 §1.5's `Vehicle` input context for the channel.

#### Prediction contract

The client that holds `Drive` predicts the vehicle. Other occupants and observers interpolate it (04 §5.3).
- **Wheeled and hover** vehicles are predicted through `SingleBodyPredictor`: the chassis body plus its
  `VehicleConstraint` or hover listener, in a client Jolt system with the static colliders and terrain
  collision tiles (02 §5.8a), in the owning bubble's coordinates, with the bubble's collision-step count
  (3 at 20 Hz).
- **Mounts** are predicted through the shared character mover, like on-foot characters.

Both follow §8.2's bit-exact rules. Each step is a pure function of `(quantized Drive input, controller
state, body state, collider set, attributes, ambientWind)`. It is bit-exact on every 09 determinism-row
toolchain, at 1, 4 and 16 workers, and between the cell and the predictor:
1. **Quantized input only**, as in §8.2 rule 1.
2. **Arithmetic.** Helios code (governor, boost, traction assist, hover controller, gait mover) runs in f64
   under strict FP with `hmath`, sums in cooked wheel, pad or gait order, and rounds once to Jolt's float
   inputs. Jolt's own vehicle code is covered by `JPH_CROSS_PLATFORM_DETERMINISTIC` (00 ADR-013).
3. **Snapshot.** The rollback snapshot holds the chassis state and the constraint's `SaveState` bytes:
   engine rpm, current gear, clutch friction, shift timers, per-wheel angular velocity, rotation and steer
   angle, and the latched driver input. It also holds boost state and energy, fuel, the auto-reverse timer
   and damage steps, and for mounts the gait, speed, stamina and jump-track tick. It is ≤ 192 B for four
   wheels and ≤ 320 B for eight, so 04 §5.2's 128 ticks cost ≤ 40 KB.
4. **Casts.** Wheel casts go through a Helios `VehicleCollisionTester` subclass that applies the hover
   tie-break (step 1 of the hover pipeline). Broadphase results are sorted by entity ID (§11 rule 1).
5. **Discrete events.** Wheel, pad and engine health are quantized to steps of 1/16, and change only on
   replicated events stamped with their tick, as in §8.2 rule 6. A destroyed wheel loses its contact; a
   destroyed pad loses its lift.
6. **Hash and correction.** Each owner snapshot carries a 32-bit xxh3 of the full predicted state at
   `last_processed_input_tick` (4 B). On a mismatch, or any discrete difference, the cell attaches the
   full-precision state (the rule 3 snapshot) to the next owner snapshot. The client restores it at that tick and
   replays. Mounts use 04 §5.2's quantized rule (> 1 cm) as well.

Corrections therefore come only from contacts with other dynamic bodies (impacts included), damage and
effects that others apply. As in §8.2, each is tagged with its cause: `contact`, `effect`, `damage`, `wind`,
`terrain` (a mismatch whose window holds only static and terrain contacts) or `divergence`. `terrain` and
`divergence` must be 0 in GP-4d. In production, a rate of either above 1 per 10⁶ owned-vehicle ticks pages
the gameplay on-call.

#### Replication, validation, ownership and cost

- **Observers** get the chassis transform and velocity at the normal rates (04 §4.4), plus `steer`, `rpm`
  (8 bits each), `gear` and `boost` for animation and audio: ≈ 26 B per update. Wheel spin and suspension
  compression are recomputed cosmetically, with one ray per wheel, in the A0–A1 animation tiers (02 §7.2).
  Interpolation follows 04 §5.3, and extrapolation stops after 150 ms.
- **Validation (04 §5.5).** Speed over ground is capped at `max(maxSpeed, boost.speed)` + 2 m/s. Boost needs
  energy. `Drive` input from a sender who does not hold the seat is dropped and counted.
- **Ownership.** A called vehicle or mount is an authority attachment of its owner, like a follower (§7.3):
  a member of the owner's AG with no fence row, handed off and recovered with the owner. Seated passengers
  join the vehicle's AG tree with `Fence.Join`, as boarders do (04 §6.1), so the vehicle and everyone in it
  change cell on one tick.
- **Call and store (§9.5).** *Call* takes `call.castSecs`. It is blocked by `State.InCombat`, inside a
  pressurized `PortalCell` unless `call.indoors`, and outside `call.zones`, and it places the vehicle with a
  shape cast within 6 m. A vehicle left idle for `idleStoreSecs`, or beyond its owner's leash, is stored
  with its health, fuel and loadout.
- **Cost** on the SERVER core: per collision step, a wheeled vehicle costs ≤ 25 µs with four cylinder
  casts, a tracked one ≤ 40 µs and a hover vehicle ≤ 12 µs with four rays. A mount costs ≤ 15 µs per
  tick, the character class. RT-20's 50 vehicles at 20 Hz take ≤ 4 core-ms per tick, ≤ 0.6 ms of stage 3's
  wall time on 8 workers.
- **Authoring.** T21 assembles chassis with wheel, pad, seat and saddle ports. It plots torque and power
  curves, the gear chart and suspension frequencies, and runs the envelope preview. T15 authors mount and
  rider animation sets and extracts gait speeds. T08 edits the numbers live in PIE, and T26 overlays rpm,
  gear, slip, suspension, pad heights and corrections by cause.

### 8.2 6-DoF flight control (Elite, SC)

A `ThrusterDef` on an item port defines mount, direction, max thrust, spool time constant, power
and fuel draw, and health. Each tick the pilot's client predicts, and the cell authoritatively
runs, this pipeline:

1. **Input.** Stick, throttle and strafe, after device curves. The `Flight` channel sends six 8-bit
   axes (strafe x, y, z; pitch, yaw, roll), an 8-bit throttle and mode buttons in the 04 §5.2 input
   record.
2. **Mode.** *Coupled* holds velocity; *decoupled* holds orientation. Assists (G-limiter, comstab,
   atmospheric auto-level) are attributes and tags.
3. **Master modes as effects (R04-P1-13).** `Mode.SCM` gives 220 m/s with shields up. `Mode.NAV`
   gives 1,000 m/s with `Ability.Weapon.*` blocked, shields suppressed and quantum travel enabled.
   Switching is a spooled ability. The Elite "blue zone" is an HXL turn-rate curve.
4. **Control.** A per-axis PID turns velocity error into a desired wrench `w ∈ ℝ⁶`.
5. **Thruster allocation.** Solve `min ||B·u − w||² + λ||u||²`, `0 ≤ u_i ≤ 1`. The warm start is a
   pseudo-inverse cached per thruster-health configuration. A bounded active-set loop (≤ 8
   iterations) handles saturation. `B` is rebuilt only when thruster state, mass or center of mass
   changes, so a damaged thruster changes handling emergently.
6. **Actuation.** Spool dynamics apply forces to the Jolt body in the parent grid. Optional
   `AeroSurfaceDef` lift and drag apply in atmosphere, against the local wind from §9.9.

**Determinism: the bit-exact contract that 04 §5.3 relies on.** 04 §5.3 has the owned hull's prediction
match the cell bit for bit, so that corrections come only from events the client cannot know. That
holds only if every stage above is bit-identical, not merely close. Stages 1–6 therefore form a pure
function of `(quantized input, controller state, body state, thruster set, attributes, ambient wind)`.
That function is **bit-exact** across the 09 determinism-row toolchains: MSVC (VS 2022 and VS 2026,
ADR-001a), clang-cl, GCC, Clang and MinGW. It is also bit-exact at 1, 4 and 16 workers, and between the
cell and the client's `SingleBodyPredictor` path (GP-4a). The rules:
1. **Quantized input only.** The controller reads the axes exactly as the input record quantizes them.
   The client quantizes before it predicts, and never feeds raw device values to the controller.
2. **Arithmetic.** f64 throughout, built with strict FP (no fast-math, no FMA contraction, 02 §2.1).
   Every transcendental comes from `hmath`. Sums over thrusters run left to right in thruster order,
   with no reassociation or SIMD reduction. The wrench is rounded to Jolt's force input once, at the end.
3. **Controller state in the snapshot.** PID integrators and derivative filters, per-thruster spool
   state, the mode and the assist tags are part of the rollback snapshot. A rollback restores them with
   the body.
4. **Thruster order.** Thrusters are indexed in the cooked `ItemPortDef` order (port ID ascending), never
   in ECS or Jolt order. `B` is 6 × N with N ≤ 64.
5. **A fixed allocation algorithm.**
   - *Warm start:* `u₀ = Bᵀ(BBᵀ + λI)⁻¹w` with `λ = 10⁻⁶ · trace(BBᵀ)/6`, so the matrix is always
     positive definite. The 6 × 6 system is solved by an unpivoted LDLᵀ factorization with a fixed loop
     order.
   - *Cache:* the factor is memoized per key `(healthSteps[N], comQ)`, with the centre of mass quantized
     to 1 mm. The memo is a pure function of its key, so a client and a cell that compute it at different
     times still get identical bits.
   - *Active set, at most 8 iterations:* find every i with `u_i < 0` or `u_i > 1`. If there is none,
     stop. Otherwise clamp **one** thruster, the most violating one (ties go to the lower index), remove
     it from the free set, subtract its contribution from `w`, and re-solve over the free set with a
     fresh LDLᵀ of the reduced normal matrix, never a cached one.
   - *Exit:* after 8 iterations, every remaining out-of-range `u_i` is clamped in index order. The only
     comparisons are exact ones against 0 and 1; no tolerance decides an exit.
6. **Saturation and damage as discrete events.** A thruster's available thrust is `maxThrust ×
   healthStep`, with health quantized to steps of 1/16. `B` therefore changes only on discrete,
   replicated events: damage, destruction, repair, mass changes (cargo, and fuel burn in 1 kg steps) and
   centre-of-mass shifts. Each arrives in the owner snapshot stamped with the tick it applies from, and
   the client rebuilds `B` at that tick in its re-simulation. A damage event costs one correction, never
   a lasting divergence.
7. **Spool.** `u_actual += (u_cmd − u_actual) · k`, where `k = 1 − hmath::exp(−dt/τ)` is computed once
   per `(τ, dt)` at record load.
8. **Ambient wind.** Aero surfaces read a piecewise-constant `ambientWind`: §9.9's base and front wind at
   the hull, quantized to 1/64 m/s per axis. The cell samples it at 1 Hz and schedules each value in the
   owner snapshot 250 ms before the tick it applies from. Clients at up to 200 ms RTT receive every change
   before they predict its tick. Above that, each wind change costs one correction. Cue wind emitters
   (§1.5) are presentation-only and never enter physics.
9. **Jolt order and caches.** Jolt orders contacts by stable body keys, never `BodyID`, and ShipHull and Vehicle bodies carry no contact cache across ticks (02 §7.1), so the collider set alone decides a step on the cell and the predictor.

Corrections therefore come only from contacts with other dynamic bodies, damage, and effects that others
apply (webs, knockback). The client tags each correction with its cause: `contact`, `effect`,
`thrusterDamage`, `wind` or `divergence`. `divergence` means a mismatch with none of the other causes in
the window. It must be 0 in GP-4a. In production telemetry, a rate above 1 per 10⁶ owned-ship ticks pages
the gameplay on-call. The allocator costs ≤ 20 µs per ship (GP-4a): 8 iterations of a 6 × 64 product and
a 6 × 6 factorization.

### 8.2a EVA and zero-g locomotion (M09, BENCH-6)

`EVA` is the movement model of a suited character whose effective gravity is below 0.05 g: open space, a
zero-g grid or a ship's exterior shell. It is a predicted 6-DoF character. The body is a Jolt
`CharacterVirtual` with a 0.45 m sphere shape, so rotation never changes its collision, with no gravity
and no floor logic. It runs through the same shared mover as on-foot movement (04 §5.2), so RT-03's
character hash covers its integration.

```
record SuitDef @table("suit") {             // a component of suit ItemDefs; values become attributes
  thrusters: ThrusterSetRef;                 // 6–12 virtual thrusters, allocated by §8.2's solver
  maxThrust: Magnitude = 480;                // N; 3 m/s² for 160 kg of suited mass
  assistSpeed: Magnitude = 8; boostSpeed: Magnitude = 16;   // m/s, relative to the current grid
  turnRateDeg: Magnitude = 90; rollRateDeg: Magnitude = 60;
  deltaV: Magnitude = 60;                    // m/s: the fuel budget (Suit.Fuel)
  o2Secs: Magnitude = 1800; lowO2Pct: u8 = 20;              // Suit.O2
  magboots: bool = true; tetherLength: Magnitude? = 20; pushOffSpeed: Magnitude = 2.5;
}
```

**Controls** (the 04 §5.2 input record, unchanged):
- **Translation.** The three move axes (forward, strafe, vertical) command a velocity in coupled mode
  (the default; ≤ `assistSpeed`, or `boostSpeed` while boosting) or a thrust in decoupled mode. A `Brake`
  button zeroes velocity relative to the grid. Coupled mode uses §8.2's PID and bit-exact allocator
  over the suit's thruster set, so GP-4a's determinism covers it.
- **Rotation.** Look yaw and pitch are targets relative to the body, which turns toward them at
  `turnRateDeg`: the body follows the look, as on foot. Free-look (held) freezes the body. Roll uses the
  `RollLeft` and `RollRight` buttons at `rollRateDeg`, so the input record needs no new field.
- **Fuel** is a Δv budget. Each tick spends `Σ u_i·F_i·dt / m`, and at zero the thrusters stop. Fuel is
  part of the predicted state, like charges (§1.4), so client and cell stop on the same tick.
- **O2** drains 1 per second whenever the character is not in a pressurized cell (§9.6). It is
  server-only and replicated to the owner. A warning cue fires at `lowO2Pct`. At zero the character gets
  §9.6's `Env.Vacuum` suffocation even while `Env.Sealed`. EVA without a sealed helmet is suffocation,
  as in §9.6. Suit lockers and a docked ship's life support refill fuel and O2 through a `Hold` verb.

**Assists.**
- **Align.** In a grid with an up axis, or within 3 m of a surface (a downward sphere cast), the body
  slerps toward grid up or the surface normal at 45°/s whenever there is no rotation input. It is a
  toggle, on by default.
- **Magboots.** With magboots engaged, the sphere within 0.35 m of a surface whose physical material has
  `Surface.Magnetic` (hull plating by default) and a relative speed ≤ 3 m/s **attaches**:
  - the character switches to the on-foot capsule path (radius 0.3 m, height 1.8 m) with up = the
    surface normal, a 0.5 g stick force along −normal, a 1.5 m/s walk and no jump;
  - up follows the ground normal at ≤ 90°/s, across convex edges up to 90°;
  - jumping, or disengaging, detaches at 1 m/s along the normal;
  - attach and detach happen on command frames on both sides, so they are predicted.
- **Grab and handholds.** `Interact.Grab` targets `Handhold` interactables and `Surface.Grabbable`
  geometry within 1.2 m. Grab is a predicted ASM ability (`SetMovementOverride{Attach, anchor}`), and
  the anchor is static in the current grid, so the cell reproduces it exactly. While attached, velocity
  relative to the grid is zero, and move input climbs hand over hand along a handhold spline at 1 m/s.
  Releasing with a direction pushes off at `pushOffSpeed`.
- **Tethers.** A tether is a one-sided distance constraint (a rope: tension only) between the suit and a
  `TetherPoint` in the same grid, `tetherLength` long and reeled at 2 m/s. Attaching is a server-validated
  `Press` verb. Tether points exist only in zero-g grids and exterior shells, and a shell's margin (25 m)
  exceeds the longest tether, so a tether never crosses a grid boundary.

**Grids and the gravity switch.**
- **Exterior shell.** A ship or station that supports EVA authors an `Exterior` host grid (02 §5.4):
  zero artificial gravity, `inertialFactor` 0, and a `GridVolume` that is the hull's convex hull grown by
  25 m (07 §2.6.4). The hull's collision shape is instanced in it as a static, like interior colliders.
  Inside the shell the ship is the frame. A character therefore keeps station on a ship flying at 300 m/s
  under 1 g (BENCH-6), and is predicted in ship-local coordinates. Ignoring the host's acceleration is a
  gameplay affordance, as it is inside. EVA characters never push hulls: the reaction of a 160 kg suit on
  a hull of 20 t or more is dropped and counted.
- **Bubble ↔ shell** are ordinary `GridVolume` transfers (enter 0.5 m inside, exit 1.0 m outside, 0.5 s
  dwell; 02 §5.4). Velocity is re-expressed and the model stays `EVA`. In open space the frame is the
  bubble (04 §5.3's `BubbleOrigin` coordinates), so crossing between ships is done while they coast; a
  suit cannot chase a hull that accelerates at 1 g.
- **Into gravity.** When a transfer puts an EVA character into a grid at 0.06 g or more (an interior host
  grid through an airlock's inner volume, or a surface bubble), the same sync-point step:
  1. keeps the sphere and position, re-expresses velocity (02 §5.3) and keeps its component along the new
     gravity;
  2. **re-orients** body up to grid up (−gravity) over `min(0.5 s, angle / 360°/s)`. The sphere makes
     the rotation collision-free;
  3. **stands** when re-orientation ends: a shape cast of the standing capsule at the target pose. If it
     is clear, the character becomes on-foot `CharacterVirtual` with `ExtendedUpdate` floor snap (0.5 m).
     If it is blocked, the character takes the crouched capsule (1.2 m). If that is also blocked, it stays
     a sphere, retries each tick and depenetrates by ≤ 0.2 m per tick;
  4. **cannot fall through.** The character is placed only at a position inside a walkable `PortalCell`
     of the grid (02 §5.1 point-in-cell) with a floor hit within 2.5 m along gravity. Otherwise it snaps
     to the nearest navmesh point within 1 m, and the `physics.eva_fallthrough_guard` counter increments.
     BENCH-6 requires it to stay 0.
- **Out of gravity.** Leaving through the outer airlock door into the shell (0 g), or climbing a surface
  bubble below 0.04 g, switches to `EVA` on the next tick with orientation and velocity kept. Engaged
  magboots try to attach at once. Between 0.04 and 0.06 g the previous model stays, which prevents
  flapping on low-gravity bodies.

**Prediction, validation and cost.** The owning client predicts its EVA character in the owning grid's
coordinates: the bubble's in open space (04 §5.3), ship-local in a shell or interior. It uses 04 §5.2's
compare rule: more than 1 cm, or any discrete difference, rewinds. Grid transfers, the gravity switch,
magboot attach and detach, grabs and tether constraints all run on command frames on both sides, so they
are predicted, not corrected. 04 §5.5's checks apply with EVA caps: speed relative to the grid ≤
`boostSpeed` + 1 m/s, and thrust only with fuel left. An EVA character costs ≤ 15 µs per tick, the same
class as on-foot movement. GP-4c is the acceptance.

### 8.3 Command flight and long-range travel

- **Command flight.** `CommandKinematic` stores `{mode: Approach|Orbit|KeepAtRange|Align|Stop|Warp,
  target, range}`. It approaches max velocity exponentially: `t₇₅ = ln(4)·I·m/10⁶`.
  Clients integrate the same commands, with periodic corrections and desync telemetry (R01-Q10).
  In tactical zones the same commands are **autopilot behaviors** feeding `NewtonianFBW`, so
  "orbit at 5 km" works everywhere, for players, AI and drones.
- **Warp and quantum travel.** A `TravelDriveDef` sets spool, alignment tolerance, minimum velocity
  fraction (EVE 0.75), speed profile, fuel, obstruction and interdiction. The cell computes a
  deterministic `s(t)`, replicated as `{from, to, t0, profile}`. Streaming prefetches along the
  path, interdiction effects can abort, and the entity reparents on arrival (R04-P1-15).

### 8.3a Player fleets: fleet warp, broadcasts and command bursts (EVE; M01, R01)

05 §1.12.1 owns the roster: fleet → ≤ 5 wings → ≤ 5 squads of ≤ 10, with a boss and fleet, wing and squad
commanders (FC, WC, SC), up to 256 members. Cells own the mechanics below. Everything is counted in zone
ticks, so it behaves the same at any `d`.

```
record FleetDef @table("flt") {                        // one per project; may lower 05 §1.12.1's caps
  caps: { wings: u8 = 5; squadsPerWing: u8 = 5; squadSize: u8 = 10 };
  warp: { gatherRangeM: f64 = 8.0e6; minRangeM: f64 = 0; maxRangeM: f64 = 1.0e5;
          alignTimeoutTicks: u16 = 60; spacingM: f64 = 500; defaultFormation: FormationRef };
  broadcasts: list<BroadcastDef>;
}
record FormationDef @table("fmn") { shape: enum { Sphere, Wall, Line, Wedge, Custom };
                                    slots: list<vec3>?;          // unit offsets, Custom only
                                    order: enum { WingSquadSlot } = WingSquadSlot; }
record BroadcastDef @table("bcd") {
  tag: TagRef;                 // Fleet.Bc.Target, .Align, .WarpTo, .NeedRepair, .NeedShield, .InPosition, .HoldFire, .TravelTo
  payload: enum { Target, Position, Self, Destination }; audience: enum { Down, Up };
  cooldownTicks: u16; ttlTicks: u16; cue: CueRef?; overviewTag: TagRef?; }
record CommandBurstDef @table("cbd") { kind: TagRef; effect: EffectRef; rangeM: Magnitude; strength: Magnitude;
  audience: enum { FleetInRange, LevelInRange } = FleetInRange; }
component FleetMembership { fleetId: u64; wing: u8; squad: u8; slot: enum { Member, SC, WC, FC };
                            rights: u32; rosterVer: u64; }    // from KV GROUP (05 §1.12.1); never predicted
```

**Fleet warp.** The intent is `FleetWarp{level: Fleet | Wing(w) | Squad(w, s), dest: EntityHandle |
CelestialRef | BookmarkRef, rangeM, formation?: FormationRef}`.
- **Validation.** The sender holds the commander slot of `level` (checked against `rights`) and is not in
  warp. `rangeM` lies in `[minRangeM, maxRangeM]`, and `dest` is in the same zone instance. A sender may
  issue ≤ 1 per 10 ticks. The intent resolves at its arrival tick `t0`.
- **Eligible members at `t0`:** members of the level in the same zone instance who are undocked, not in warp,
  not tethered, with a travel drive online, not `State.WarpScrambled`, outside every interdiction volume, and
  within `gatherRangeM` of the commander. Each other member of the level gets `FleetWarpRefused{reason}` on
  the HUD and does not move.
- **Slots.** Eligible members are sorted by (wing, squad, slot rank, character ID), and member i takes the
  formation's slot i. The anchor is the point `rangeM` short of `dest` on the line from the commander, and
  that line is the formation frame's forward axis. Slot positions are the unit offsets × `spacingM`, where
  the spacing is raised to at least twice the largest member's collision radius. They are computed in 04 §5.4's
  64-bit fixed point, so the cell and every client get identical positions and no two ships land overlapping.
- **One landing tick.** Each ship aligns with its own agility: `t_align,i` is the ticks it needs to point at its
  slot within its drive's alignment tolerance at the minimum velocity fraction (§8.3; 0.75 in EVE). All ships
  enter warp together at `t_w = t0 + max_i t_align,i`. A ship that cannot align within `alignTimeoutTicks` is
  refused and keeps its slot empty. The fleet warps at the **slowest** member's warp speed (EVE's rule). Every
  ship's `s(t)` profile is stretched to the same duration `D`, the longest path's duration at that speed, so a
  shorter path flies slower and no ship exceeds its drive. Every ship lands on `t_land = t_w + D`, in its slot.
- **Replication.** Each ship gets an `Align` and then a `Warp` command `{from, to, t0, profile}` (04 §5.4),
  ≈ 20 B each. A 250-ship warp therefore costs each observer ≈ 10 KB once, paced by the priority accumulator.
- **Multi-cell zones.** The commander's cell sends `FleetWarpPlan{fleet, t0, t_w, t_land, slots[], planHash}` on
  cell↔cell trunks to every cell that owns an eligible member, with `t0` ≥ 2 ticks ahead on the zone leader's
  shared schedule (04 §3.5). An owning cell may still refuse a member (scrambled since the plan), whose slot
  then stays empty; the plan is never reshuffled. Fleet-battle zones are single-cell (04 §3.4), so this path
  serves open space.
- **In flight,** interdiction aborts per ship (§8.3), and the rest land on time. Each ship reparents on
  arrival. Travel between zones is not a fleet warp: gates and jumps stay per ship.

**Broadcasts.** The intent is `FleetBroadcast{def: BroadcastDef, target?: EntityHandle, pos?: WorldPos}`.
- **Validation.** Broadcasts are schema'd and rate-limited: ≤ 1 per `cooldownTicks` per (sender, def) and
  ≤ 5 per 10 s per sender. A `Down` broadcast needs a commander slot and reaches that commander's level. An `Up`
  broadcast (need repair, need shield, in position) reaches the sender's squad and every commander above it. A
  target must be in the sender's replicated set.
- **Delivery.** At the arrival tick, the sender's cell publishes a structured message on the fleet's chat
  subject `chat.<shard>.fleet.<id>` (05 §2.2). It is not player text, so it skips the text filter. Each gateway
  holds the roster, as it does for voice, and delivers the message on EVENT_R to its sessions in the audience.
  Latency is ≤ 1 tick to the next tick boundary, plus one gateway hop, plus RTT/2: **≤ 1 tick + RTT** at any
  `d`.
- **Client.** Overview highlights by `overviewTag`, a cue, one-click actions (lock, align, warp to) and a
  history list in the Fleet panel (08 §1.7.2). A broadcast from another zone shows the zone's name and offers
  only TravelTo. Broadcasts expire after `ttlTicks`.

**Command bursts.** A `CommandBurstDef` module is an ability on a ship (§1.4). On activation at tick t:
- **Recipients** are the fleet members (the whole fleet, or the booster's level for `LevelInRange`) in the same
  zone instance whose ships are within `rangeM` of the booster at t, the booster included. The set is a
  snapshot, like EVE's pulse. It is the `Fleet` domain of §1.2's cross-entity modifiers.
- **Stacking.** Each recipient gets `effect` at the burst's `strength` for a zone-clock duration. The effect uses
  `stacking {policy: ByTarget, limit: 1, keep: Strongest}` keyed by the burst's `kind`: an incoming burst
  replaces the active one only when its strength is ≥ the active one's, restarting the duration, and is
  otherwise ignored. Different kinds stack normally. Burst modifiers use their own `penaltyGroup`, so they
  never penalize module bonuses.
- **Leaving.** A recipient that leaves range after t keeps the effect until it expires, as in EVE; one that
  leaves the fleet loses it at the next tick boundary.
- **Multi-cell zones.** The booster's cell sends idempotent `ApplyEffect` messages to the owners of remote
  recipients, resolved from ghosts at t (§8.7).

**Killmails.** Each attacker row records the `fleetId` and slot it held at the kill tick, and the killmail
carries `fleets[]{fleetId, damageShare}`. The killmail viewer filters by fleet (08 §1.7.2), and org
killboards count fleet kills. Drone damage counts for its owner's fleet, and `Perm.Follower.Command` drone
assist accepts any member of the owner's fleet (§7.3).

**Bots.** `helios-bot` (04 §10.3) gains a fleet-commander behaviour: it forms a fleet from an advert, calls
targets and positions, fleet-warps and runs bursts. NS-4.2's battle and `starter-fleet`'s proof fight as player
fleets.

### 8.4 Multi-crew seats, stations and item ports

- **Item ports.** `ItemPortDef` hierarchies on vehicle and structure prefabs have a name, accepted
  `TagQuery`, size, transform and child ports. An installed item's ledger location is
  `(vehicleItem, flag = portId)`, so fitting, persistence and loot share one tree (R04-P1-14).
- **Seats and stations.** `SeatDef`/`StationDef` grant **input channels**: `Flight`, `Drive`
  (ground vehicles and mounts, §8.1a), `TurretGroup[n]`, `Engineering`, `Scanner`, `Comms`. Enter and exit keep one continuous camera
  (ADR-010). Access is gated by owner, org crew roles or party ACLs. The cell routes each channel to
  a player or an AI brain (including hired crew, §7.3), so AI crew press the same inputs. The pilot
  predicts the ship and the driver predicts the vehicle; a gunner predicts only the turret. The owner-level `Followers` channel (§7.3) is
  routed the same way.

### 8.5 Ship systems, sensors and EWAR

- **Resource-network solver** (1–4 Hz per ship). It works over component items' power, heat,
  health and state attributes. Power is allocated by pips or priority. Heat follows
  `dT = (gen − cooling)/capacity`, and overheat effects damage components. The EVE capacitor curve
  `dC/dt = (10·Cmax/τ)(√(C/Cmax) − C/Cmax)` is HXL. Phase 2 is a simple budget. Relays, fuses and
  hot-swappable components come in Phase 4 (R04 §6.2).
- **Shields.** `ShieldDef` gives either N directional faces or a bubble, with per-type resists and
  regen delay and rate.
- **Sensors (R09-G5).** Signatures (`SignatureRadius`, `EmEmission`, `IrEmission`, `CrossSection`)
  and sensors (`SensorStrength`, `ScanResolution`, `LockRange`, `MaxTargets`) are attributes. Each
  1–5 Hz, spatially bucketed sensor tick builds contact lists with an HXL `detect(emission,
  distance, strength, ctx.visibility)`; weather sets `ctx.visibility` (§9.9).
- **Locking** is an ability with duration `40000/(ScanResolution·asinh(sig)²)` (R09-A2).
- **EWAR is effects** with stacking penalties. Webs apply `PostPercent` to velocity, damps reduce
  `LockRange`, scramblers grant `State.WarpScrambled`, and ECM rolls `P = jam/sensor` each cycle and
  grants `State.Jammed`, which blocks locks.

### 8.6 Delivery and resolution strategies

Each ability chooses a strategy. All strategies feed one damage pipeline (R09-A3, R09-G4).

| Strategy | Server resolution | Use |
|---|---|---|
| `Validate` | Range, LOS, facing, resources; roll hit, crit, mitigation | Tab-target |
| `Statistical` | HXL chance (`TurretHitChance`) | EVE turrets |
| `RewindHitscan` | Trace against hitbox history at shooter time, rewind capped at 200 ms by default | FPS, ship guns |
| `Projectile` | Predicted spawn, validated against rewound hitboxes | Slow weapons |
| `Missile` | Entity with guidance, lock type, countermeasure tags | Space |
| `Shape` | `TelegraphShapeDef` (circle, cone, rect, ring, line) evaluated at resolution; clients draw it at cast start | Action combat, bosses (R03-P1-10) |
| `Beam` | Continuous trace at zone tick | Mining lasers |

**Combat modes are configurations, not separate engines.**
- **Tab-target:** GCD groups, a queue window, a toggle auto-attack and threat.
- **Action:** `Shape` resolution plus timed `State.Invulnerable.Dodge` i-frames.
- **FPS:** a `WeaponDef` sets fire modes, RPM, recoil, spread and bloom, falloff and `HitZone.*`
  multipliers.
- **Aim assist (R05-P0-9):** an `AimAssistDef` per weapon × device (mouse, pad, HOTAS) sets the
  magnetism cone, friction, slowdown and an airborne multiplier. The server validates hits with the
  same cone.
- **PvE hit reports:** client-reported PvE hits are a per-activity flag, never PvP (R05 §4.5).

### 8.7 Damage model and death pipeline

```
struct DamagePacket { EntityId source, instigator; RecordId ability; TagSet tags;
                      FixedArray<{TagIndex type; f32 amount}, 4> parts; TagIndex hitZone;
                      vec3 point, dir; u32 predictionKey; u64 idempotencyKey; };
```

`DamageModelDef` (per record template) runs these steps in order:
1. The attacker's `DamageExecution` applies crit and multipliers, then immunity tags are checked.
2. **Ordered layers** absorb damage: the shield face chosen by `dir`, then armor, then hull (or
   health). Each applies `absorbed = min(pool, amount·(1 − resist[type]))` and passes the rescaled
   overflow on.
3. **Subsystem routing** sends a share to the component matching the hit-zone tag (R09-A2). Hull
   damage to an interior cell's hull section can breach it (§9.6 pressure).
4. `DamageApplied` and `PoolDepleted` events feed cues, threat, contribution, quests and
   killmails.

Packets that cross authority groups arrive as `ApplyDamage` messages with their idempotency key. A
follower's packets carry its owner as `instigator` (§7.3).

**Death pipeline (R09-A17, R09-G11).** `DeathPipelineDef` is chosen per record template and
`ZoneRulesDef`. It runs: optional incapacitation with a revive window → death →
`onVehicleDestroyed: ejectTo` a capsule (EVE pod) → per-item drop, destroy or keep rolls (seeded,
audited, one ledger transaction) → a wreck entity with loot and a lifetime → a `Killmail` event →
respawn selection from `RespawnPointDef` candidates (bind points, clone bays, medbeds) → penalty
effects (wounds, implant loss) → an insurance claim paid on a durable timer. The owner's death stores
summoned companions and pets; launched drones follow the abandon rule (§7.3).

## 9. Social and world systems

### 9.1 Guilds and corporations

- The Go social service owns membership and roles (05 §1.11).
- Permission names (`Perm.Hangar.Div3.Take`, `Perm.Structure.Dock`, `Perm.Follower.Command`) are
  declared in the schema and map to bits of its 128-bit masks.
- Cells evaluate `Perm.Check(actor, resource, action)` against the `PERMS` KV cache, using ACLs
  that reference org roles, so every service shares one RBAC model (`pkg/perm`, R09-A11).
- Org wallets and hangars are ledger owners.

### 9.2 Structures, housing and cities (G07; R02 §6.4, R02-Q12, R03-P1-9)

SWG's houses, guild halls, harvesters, factories and player cities, EVE-style deployables and SC-style
outposts share one structure model. Every piece of its state has exactly one owner:

| State | Owner and store | Written by | Reaches cells and clients through |
|---|---|---|---|
| The structure, its interior contents and decoration transforms | Ledger items (05 §1.6) in the custody of the structure's persistent AG | its owning cell under the fence, or a service with the dormant precondition while it is parked (05 §4.1) | the cell's inventory cache and checkpoint ("ledger wins", 05 §1.13) |
| The ground it occupies | Ledger plot rows, exclusion-constrained (05 §1.6) | the `OccupyPlot` and `VacatePlot` legs of the placing or removing transaction | the zone's `ZoneStructures` manifest (§9.2.1) |
| Lot budget | Ledger: the bound currency `Lot` | `Transfer` legs of the same transactions | the wallet cache |
| Upkeep pool and upkeep stage | Ledger wallet `upkeep:<structure>`, plus the Industry service's `structure_upkeep` row (05 §1.8) | owner deposits (cell); the upkeep worker (durable timers) | `evt.<shard>.structure.stage` |
| City, citizens, elections, treasury | The Foundation world script `CityGovernance` (§9.2.4; 05 §1.23) | WSH invocations only | the keyed world flag `zone.<zone>.City.<id>` in KV `WORLD` (05 §1.21); RPCs through a cell |
| Territory lifecycle, influence, sovereignty | World State service (§9.3; 05 §1.21) | version-CAS transitions, audited | KV `WORLD` `zone.<zone>.Territory.<id>` |

#### 9.2.1 Structure records and placement

```
record StructureDef @table("str") {
  kind: enum { House, GuildHall, Harvester, Factory, Deployable, Civic, CityHall, Territory };
  footprint: Polygon2;                 // structure-local metres; r_fp is its bounding-circle radius
  clearanceM: f32;                     // this side's gap: two plots keep clearanceA + clearanceB apart
  maxSlopeDeg: f32 = 12; maxHeightDeltaM: f32 = 1.5;
  lotCost: u8;                         // 0 for Civic, CityHall and Territory
  placeRules: TagQuery;                // zone and region tags that allow it (Zone.Structures, not Region.NoBuild)
  cityRule: enum { Anywhere, OutsideCities, InsideCityOnly, Civic } = Anywhere; civicRankMin: u8 = 0;
  terrainStamp: TerrainStampSpec;      // Flatten or Road, extent ≤ r_fp + 16 m (02 §5.8 stamps)
  interior: InteriorRef?; itemCap: u16 = 0;
  upkeep: UpkeepSpec?;                 // absent for Civic and CityHall: the city pays (§9.2.4)
  power: f32 = 0; lifecycle: StructureLifecycleRef?;   // Territory only (§9.3)
  client { ghost: AssetRef<Prefab>; }
}
struct UpkeepSpec { perHour: Money; poolMax: Money; settleHours: u16 = 24; decayStages: u8 = 3;
  stageHours: u16 = 72; condemnDays: u16 = 30; restoreFee: Money; reclaimRetentionDays: u16 = 180; }
```

**Placement rules.** `PlacementCheck(def, pose, inputs) → {verdict, reason, samplesHash}` is one pure C++
function in `engine/gameplay`. The cell runs it authoritatively, and 08's placement ghost runs it as an
advisory pre-check (08 §1.7.2). Its inputs are only cooked or replicated data, so both sides see the same:
- cooked zone data: `ZoneRulesDef` tags, no-build polygons (NPC towns, roads, spawn lairs, 64 m around travel
  points), and cube-face and zone edges;
- terrain heights from the deterministic layer stack, runtime stamps included (W04, 02 §5.8);
- the zone's `ZoneStructures` manifest (below) and its `CityFootprint`s (§9.2.4);
- actor facts: the `Citizenship` component (§9.2.4) and the lot and credit balances.

| # | Rule | Rejects with |
|---|---|---|
| 1 | The zone and region tags match `placeRules`; the zone allows the kind (`ZoneRulesDef.structures`, `.cities`) | `ZONE_RULE` |
| 2 | The footprint is outside every no-build polygon and ≥ 64 m inside the zone edge, and the plot lies on one cube face | `NO_BUILD`, `EDGE` |
| 3 | Heights are sampled on a 1 m grid over the footprint (≤ 1,024 samples) and quantized to 1 cm integers. The least-squares plane's slope, compared in fixed point against a cooked tan table, is ≤ `maxSlopeDeg`; max − min ≤ `maxHeightDeltaM`; no sample is below the water level | `SLOPE`, `HEIGHT`, `WATER` |
| 4 | The plot circle `(centre, r_fp + clearanceM)` overlaps no plot in `ZoneStructures` | `CLEARANCE` |
| 5 | City rules, against every footprint that contains the plot centre: `OutsideCities` kinds are refused inside a city and `InsideCityOnly` kinds outside; inside a zoned city the actor needs that city's `Citizenship` or a zoning permit; `Civic` kinds need the mayor role and `rank ≥ civicRankMin`; a `CityHall` needs ≥ `CityDef.minCenterSpacingM` to every other city centre in the zone; an actor in the footprint's ban filter is refused | `CITY_*` |
| 6 | The zone holds fewer than `ZoneRulesDef.maxStructures` structures (3,000 on a Harrow-sized surface zone) | `ZONE_FULL` |
| 7 | Lot balance ≥ `lotCost`, and credits ≥ the fee | `LOTS`, `FUNDS` |

Every comparison uses integers or `hmath` strict FP, so the verdict and `samplesHash` are bit-identical on every
determinism toolchain (ADR-001a rule 7). GP-16 (a) measures parity.

**Commit.** The cell that owns the region holding the plot centre re-runs the check. Inside a zoned city it
then asks `CityGovernance.AuthorizePlacement` for the exact ban list and permits; this fails closed with
`CITY_UNAVAILABLE` while the WSH is down. It then issues one ledger transaction, idempotency key
`place:<deed>`:
- `Burn` the placement fee (`Sink.Structure.Placement`; for a `CityHall`, also `CityDef.foundingFee` as
  `Sink.City.Founding`);
- `Transfer` `lotCost` of `Lot` from the character to the structure (`Transfer.Lot.Occupy`);
- `Modify` the deed item into the placed structure at `(zone, pose)`, and `CreateAg` its persistent AG;
- `OccupyPlot(zone, face, plot circle, structure, maxStructures)` (05 §1.6);
- optionally, a first `Transfer` into the upkeep pool (`Transfer.Upkeep.Deposit`).

The ledger's plot constraint, not the cell, is the arbiter. Two cells that each see a free spot across a
region seam both pass rule 4, exactly one transaction commits, and the other gets `PLOT_TAKEN`. `Lot` is a
ledger currency, so concurrent placements, returns and ownership transfers can never overspend it
(`balance ≥ 0`). The structure spawns on the ledger reply, and its terrain stamp applies at the next tick
boundary. The Industry service creates its `structure_upkeep` row from the ledger event (05 §1.8).

**`ZoneStructures` manifest.** A zone-singleton replicated component, audience the zone, lists every plot:
`(structureId, def, face, centre, radius, stamp params, stage)`, ≤ 48 B each, so ≤ 144 KiB at 3,000
structures (zstd on join, deltas after). Every cell hosting a region of the zone loads it from the ledger's
plot rows (`ListPlots(zone)`) and keeps it current from the plot ops in `evt.<shard>.ledger.tx`, which cells
already consume for their inventory caches (05 §1.6). Stamps apply in structure-ID order (IDs are
time-prefixed, 05 §1.4.5), so every cell and client builds the same terrain, collision and nav, and a house
2 km away flattens the ground even for a client whose interest set does not hold the house. A plot stays in
the manifest in every upkeep stage until reclamation.

**Housing zones.** A zone whose `ZoneRulesDef` allows structures never opens overflow layers (04 §7), because a
plot must exist exactly once; the zone cook rejects the combination. Such zones scale by static multi-cell
partitioning (S02). House and ship interiors are 04 §7 housing instances that park on `idle_ttl`.

**Lots.** `Lot` is a bound, untradable, integer `CurrencyDef`. The character-creation transaction mints
`CharacterDef.baseLots` (10, as in SWG) with `Faucet.Lot.Base`, and progression may grant more with
`Faucet.Lot.Skill`, up to 12. Lots return on pack-up, reclamation and ownership transfer
(`Transfer.Lot.Release`). An ownership transfer moves the structure and its lots in one two-party transaction,
which fails if the recipient lacks the lots.

#### 9.2.2 Decoration, access and vendors

- **Decoration** stores item transforms in the instance payload, within interior bounds and `itemCap`. The
  structure's owning cell is the only writer of its interior (single custody, 05 §4.1). It counts the cap in
  the tick that issues each ledger transaction, including slots reserved by transactions still in flight, so
  N admins dropping items at once cannot overshoot, and a failed transaction frees its slot. A GM restore into
  a parked structure goes through the admin API, which refuses one that would exceed the cap.
- **ACLs** cover admin, entry, ban and vendor rights. An entry may name a character, an org role or a city
  role (`City.<id>.Mayor`, `.Militia`, `.Citizen`, read from the `Citizenship` component).
- **Player vendors** count as one item. Their listings are escrowed Market listings capped by
  `VendorDef.maxListings` (§4). A vendor closes at condemnation: its listings are cancelled through the Market
  service, and their escrowed items return to the vendor owner's hangar.

#### 9.2.3 Upkeep: maintenance, decay, condemnation and reclamation

The Industry service's upkeep worker (05 §1.8) owns the chain. As with harvesters (§3), accrual is analytic
and only transitions are timers.

| Stage | Entered when | Effect | Left when |
|---|---|---|---|
| Paid | the pool covers accrued upkeep | none | the pool runs dry at `paidThrough` |
| Decay 1…N (N = `decayStages`) | at `paidThrough`, then every `stageHours` (72 h) | condition −25 % per stage (`Structure.Decay.k`, cosmetic damage); debt accrues | a deposit covers the debt → Paid |
| Condemned | `stageHours` after Decay N (9 days of arrears by default) | entry is locked except for the owner's *Pay debt* verb; vendors close; a residence stops counting for citizenship; lots and plot stay held | the owner pays the debt plus `restoreFee` → Paid; or `condemnDays` (30) pass → Reclaimed |
| Reclaimed | `condemnDays` after condemnation | one ledger transaction (below) | final |

- **Settle.** A timer `upkeep:<structure>:<seq>` fires at the earlier of `paidThrough` and `settleHours`
  (24 h). The worker runs one ledger transaction, key `upkeep:<structure>:<seq>`, guarded by
  `ClaimGuard('upkeep', <structure>:<seq>, week)`. It burns the accrued upkeep from the pool
  (`Sink.Maintenance.Structure`) and, when the plot centre lies in a city, adds the property-tax and
  residency legs as claims into `CityGovernance`'s escrow (§9.2.4). The row's `seq`, stage and next timer
  then commit in one PG transaction. A crash between the two replays the settle under the same key, so each
  settle, stage change and tax leg happens once.
- **Deposits and repayment** are cell ledger transactions from the owner's wallet into the pool, bounded by
  `poolMax` (`Transfer.Upkeep.Deposit`). The worker consumes the deposit's ledger event and runs an immediate
  settle under the next `seq`. When the pool now covers the debt, that settle burns it (and, for a condemned
  structure, `restoreFee` as `Sink.Structure.Restore`) and returns the stage to Paid; condition damage stays
  until repaired with an `Interact.Repair` verb (§9.6).
- **Reclamation** is one ledger transaction, key `reclaim:<structure>`, guarded by the structure item's
  state:
  - every interior item, and the structure itself as a packed deed, moves into one new reclaim container at
    the owner's reclaim location, at rest and claimable at any reclaim terminal;
  - `VacatePlot` frees the ground, and the lots return to the owner;
  - the stamp leaves the manifest on the ledger event.

  For a loaded structure the worker asks the owning cell, which issues the transaction under its fence. For a
  parked one the worker issues it with the `{ag, epoch, dormant}` precondition (05 §4.1). A reclaim container
  unclaimed after `reclaimRetentionDays` (180) is destroyed with `Sink.Reclaim.Expired` by the lifecycle job
  (05 §1.13).
- **Pack-up** by the owner is the same transaction, run at once, with the container delivered to the owner's
  hangar.
- **Stage projection.** Stage changes publish `evt.<shard>.structure.stage`. The hosting cell applies them at a
  tick boundary, idempotently by `seq`, and a cell that loads a zone reads its structures' stages in one batch.
  `CityGovernance` consumes the same events for residences and civic structures.

#### 9.2.4 Cities: the `CityGovernance` world script

Player cities are critical for SWG and their rules differ per game, so they ship in the Foundation layer
(01 §4.1) as a world script (05 §1.23), not as engine C++ or a Go service. A studio overrides the script or
its `CityDef` records without editing backend source. Its tables, timers and escrow get the WSH's exactly-once
invocations, lease fencing, hot swap and audits.

```
record CityDef @table("cty") {
  ranks: list<{ name: LocString; minCitizens: u16; radiusM: u16; civicUnlocks: TagSet; maxCivic: u8 }>;
      // ranks 1…5, default (SWG): Outpost 5 / 150 m, Village 10 / 200 m, Township 15 / 300 m,
      // City 30 / 400 m, Metropolis 40 / 450 m; a Founding city uses rank 1's radius
  foundingDays: u16 = 21; cycleDays: u8 = 7; downgradeCycles: u8 = 2; minCenterSpacingM: u16 = 1_000;
  termWeeks: u8 = 3; candidateMinDays: u8 = 14; voterMinDays: u8 = 7; oneCitizenPerAccount: bool = true;
  taxBounds: { property: 0–50 %; residency: 0–2,000 cr/week; sales: 0–20 %; travel: 0–500 cr; service: 0–20 % };
  civicUpkeepPerWeek: map<StructureDefRef, Money>; civicCondemnWeeks: u8 = 3; disbandArrearsWeeks: u8 = 5;
  withdrawPerDayPct: u8 = 10; foundingFee: Money;
}
```

`minCenterSpacingM` is at least twice the largest radius plus a buffer (2 × 450 + 100 m), so a rank-up can
never make two cities overlap, and radius growth needs no neighbour check. Structures that a growing radius
engulfs are grandfathered: they pay the city's property tax, and their owners do not become citizens.

**Civic structures.** A `CityHall` and every `Civic` structure belong to the system owner `city:<id>`, so a
change of mayor moves no items. Their ACL admin is the city's `Mayor` role, they hold no lots, and the treasury
pays their upkeep in the weekly cycle. The mayor places at most `ranks[rank].maxCivic` of them, and only kinds
in the rank's `civicUnlocks` (a shuttleport, garage, cloner, cantina or garden in the SWG defaults).

```
worldscript CityGovernance @version(1) @partitions(4) {
  escrow Credits;                                                  // every city's treasury
  table City @key(id: u64) @partitionKey(zone) @index(zone, status) @index(status, nextCycleAt)
             @escrowBacked(treasury, when: status != Disbanded) @maxRows(50_000) {
    def: RecordId; zone: ZoneId; center: PlotPos; name: EncryptedText(32); founder: CharacterId @subject;
    status: enum { Reserved, Founding, Active, Disbanded }; rank: u8; lowCycles: u8; citizens: u32;
    mayor: CharacterId? @subject; hall: StructureId; civic: list<StructureId>; civicArrears: u8;
    treasury: i64 @currency(Credits); taxes: TaxRates; pendingTaxes: TaxRates?; zoning: bool;
    withdrawnToday: i64; footprintVer: u64; nextCycleAt: Time; term: u32; reservedUntil: Time?;
  }
  table Citizen @key(city: u64, key: ResidentKey) @partitionKey(zone) @index(city, since) @index(residence)
                @ttl(leftAt + 7d) {
    zone: ZoneId; character: CharacterId @subject; residence: StructureId; since: Time;
    role: enum { Citizen, Militia, Mayor }; seq: u64; leftAt: Time?;   // set = a tombstone
  }
  table Residence @key(key: ResidentKey) @partitionKey(key) {      // account or character, per CityDef
    character: CharacterId @subject; city: u64; zone: ZoneId; structure: StructureId; seq: u64;
  }
  table Election @key(city: u64, term: u32) @partitionKey(zone) @index(status, closesAt) {
    zone: ZoneId; status: enum { Candidacy, Voting, Settled }; closesAt: Time;
    candidates: list<CharacterId @subject>; winner: CharacterId? @subject; tally: list<u32>;
  }
  table Ballot @key(city: u64, term: u32, voter: ResidentKey) @partitionKey(zone)
               @index(city, term, candidate) @ttl(castAt + 90d) {
    zone: ZoneId; candidate: CharacterId @subject; castAt: Time;
  }
  table Permit @key(city: u64, character: CharacterId, kind: enum { Zoning, Ban }) @partitionKey(zone)
               @ttl(expiresAt) { zone: ZoneId; expiresAt: Time; }
  table Journal @key(city: u64, seq: u64) @partitionKey(zone) @ttl(at + 180d) {   // public city journal
    zone: ZoneId; reason: ReasonRef; amount: i64; actor: CharacterId? @subject; at: Time;
  }
  rpc Reserve(ReserveCity) -> Reservation        @callers(cell) @partitionBy(zone) @rate(1 per 10 min per character);
  rpc Found(FoundCity) -> CityId                 @callers(cell) @partitionBy(zone);
  rpc AuthorizePlacement(PlaceQuery) -> Verdict  @callers(cell) @partitionBy(zone) @readOnly;
  rpc DeclareResidence(Declare) -> Ack           @callers(cell) @partitionBy(key) @rate(1 per 24 h per character);
  rpc Deposit(DepositClaim) -> Ack               @callers(cell) @partitionBy(zone);   // cell-side taxes, donations
  rpc SetPolicy(Policy) -> Ack                   @callers(cell) @partitionBy(zone) @rate(20 per day per character);
  rpc Withdraw(Withdrawal) -> Ack                @callers(cell) @partitionBy(zone) @rate(1 per day per character);
  rpc Stand(Candidacy) -> Ack                    @callers(cell) @partitionBy(zone);
  rpc Vote(CastBallot) -> Ack                    @callers(cell) @partitionBy(zone) @rate(10 per h per character);
  rpc MyCitizenship(ResidentKey) -> CitizenshipView @callers(cell) @partitionBy(key) @readOnly;
  rpc View(CityQuery) -> CityView                @callers(cell, gm, public) @readOnly @cacheSecs(30);
  on event ws.CityGovernance.residenceChanged @partitionBy(zone);  // Residence partition → each city's partition
  on event ws.CityGovernance.residenceLapsed  @partitionBy(key);   // city partition → Residence partition
  on event ws.CityGovernance.claim            @partitionBy(zone);  // upkeep-worker tax claims (05 §1.23 item 6)
  on event structure.stage                    @partitionBy(zone);
  timer Cycle(CityId); timer FoundingDeadline(CityId); timer ReservationExpiry(CityId);
  timer ElectionClose(CityId, u32);
  reason Transfer.Tax.City.Property: EscrowOpen; reason Transfer.Tax.City.Residency: EscrowOpen;
  reason Transfer.Tax.City.Sales: EscrowOpen;    reason Transfer.Tax.City.Travel: EscrowOpen;
  reason Transfer.Tax.City.Service: EscrowOpen;  reason Transfer.City.Donation: EscrowOpen;
  reason Sink.City.CivicUpkeep: Sink; reason Sink.City.Disband: Sink; reason Transfer.City.Withdrawal: EscrowRelease;
}
```

`ResidentKey` is the account ID (a pseudonymous identifier, not `@pii`) when `oneCitizenPerAccount` is set,
and the character ID otherwise.

**Founding.**
1. The founder places a `CityHall` (§9.2.1). Before the ledger transaction the cell calls `Reserve{zone,
   centre, def}`. The zone's partition serializes founding: it checks `minCenterSpacingM` against every
   Reserved, Founding and Active city in the zone and writes a `Reserved` row that `ReservationExpiry` deletes
   after 10 min unless it is confirmed. It returns the new city's ID, and the placement transaction makes
   `city:<id>` the hall's owner.
2. After the placement commits, `Found{reservation, hall}` makes the row `Founding`, sets the founder as
   mayor and first citizen, arms `FoundingDeadline` (`foundingDays`) and the first `Cycle`, and projects the
   footprint. A failed placement lets the reservation expire.
3. At the deadline a city with ≥ `ranks[1].minCitizens` citizens becomes Active at rank 1 (Outpost).
   Otherwise it disbands.

**Citizenship.** A character declares residence at a House or GuildHall it owns or administers, inside a city
and in stage Paid, through the structure terminal:
1. The cell calls `DeclareResidence` on the `Residence` partition of the resident key. It writes or moves the
   row, bumps `seq`, and emits one `residenceChanged{key, city, zone, Join | Leave, seq}` per affected city,
   each routed to that city's zone partition.
2. A city partition applies the event only if its `seq` is newer than the `Citizen` row's. A `Join` writes a
   live row. A `Leave` turns the row into a tombstone (`leftAt`), kept for 7 days so that a late, older `Join`
   is ignored. `citizens` counts live rows.
3. The cell sets the character's `Citizenship{city, role, since}` component (owner-replicated, persisted in
   the character's checkpoint) from the reply. It refreshes it with `MyCitizenship` at login and at every
   city-terminal session, and applies the `ws.CityGovernance.roleChanged` broadcast to characters it hosts.
4. A residence lapses when its structure is condemned, packed up, reclaimed or transferred. The city
   partition handles the `structure.stage` event, tombstones the `Citizen` row found through its `residence`
   index and emits `residenceLapsed`, which deletes the `Residence` row if its `seq` still matches.

With `oneCitizenPerAccount` the key is the account, so alts cannot pad a city (SWG's alt-city lesson). After
quiescence live `Citizen` rows and `Residence` rows form a bijection, which GP-16 (d) audits.

**Weekly cycle.** `Cycle` runs once per city every `cycleDays`, spread over the week by city ID:
1. **Rank.** The city goes up one rank if `citizens ≥ ranks[rank+1].minCitizens`, and down one only after
   `citizens < ranks[rank].minCitizens` for `downgradeCycles` (2) consecutive cycles. The radius follows the
   rank.
2. **Taxes.** Rates the mayor set during the week take effect (one cycle of notice).
3. **Civic upkeep.** If the treasury covers it, `sink(escrow, Σ civicUpkeepPerWeek, Sink.City.CivicUpkeep)`.
   Otherwise `civicArrears` rises by one, and civic structures decay one stage per week in arrears, shown in
   the footprint. At `civicCondemnWeeks` (3) they are condemned and their unlocks switch off. At
   `disbandArrearsWeeks` (5), or after two cycles with no citizens, the city disbands: the footprint is
   deleted, the treasury is sunk with `Sink.City.Disband`, and civic structures go through §9.2.3's
   reclamation with the city's system owner as recipient and a retention of 0.
4. **Elections.** Every `termWeeks` the cycle opens an election (below).
5. **Projection.** It writes the new footprint and arms the next `Cycle`.

**Elections.** Citizens of ≥ `candidateMinDays` stand with `Stand` during a 7-day candidacy stage. Voting then
runs for 7 days and closes at `ElectionClose(city, term)`. A voter must have been a citizen for ≥
`voterMinDays`. The `Ballot` key `(city, term, voter)` makes a second vote a revote, never a second ballot.
The close invocation counts each candidate's ballots through the `(city, term, candidate)` index (≤ 16
candidates), and in one commit writes the tally, sets `status = Settled` and the new mayor, and emits
`mayorChanged` and `roleChanged`. The timer ID is the invocation ID and `status` is its guard, so a WSH kill or
a redelivered timer settles the election exactly once, and a replay recomputes the same tally from the same
rows. Ties go to the incumbent, then to the earliest candidacy. With no candidate the incumbent stays; with
neither, the city is mayorless and its policies freeze until the next term.

**Treasury and taxes.** The treasury is the city's share of the script's escrow (`@escrowBacked`), so the
hourly escrow-backing audit (05 §1.23) checks Σ `treasury` against the escrow balance. Rates are clamped to
`CityDef.taxBounds` and to the city-scoped `TaxPolicyDef` (§4).

| Flow | Payer | Collected by | Ledger path | Reason code |
|---|---|---|---|---|
| Property tax, a percentage of each settle's upkeep | the structure's upkeep pool | the upkeep worker, per settle | an escrow-claim leg in the settle transaction, delivered as `ws.CityGovernance.claim` (05 §1.23 item 6) | `Transfer.Tax.City.Property` |
| Residency levy (SWG's income tax), flat per House per week | the House's upkeep pool | the upkeep worker, pro rata per settle | as above | `Transfer.Tax.City.Residency` |
| Sales tax on vendor sales inside the radius | the buyer | the buyer's cell, in the purchase transaction | `EscrowOpen` leg, then `World.call("CityGovernance.Deposit")` | `Transfer.Tax.City.Sales` |
| Travel and service fees (shuttleport, garage, cloner) | the user | the user's cell, at the civic terminal | as above | `Transfer.Tax.City.Travel`, `.Service` |
| Donations | any character | a cell, at a city terminal | as above | `Transfer.City.Donation` |
| Founding fee | the founder | the placement transaction (§9.2.1) | `Burn` | `Sink.City.Founding` |
| Civic upkeep | the treasury | the weekly cycle | `sink` intent | `Sink.City.CivicUpkeep` |
| Mayor withdrawal, ≤ `withdrawPerDayPct` of the treasury per day, on the public journal | the treasury | `Withdraw` | `release` to the mayor's wallet | `Transfer.City.Withdrawal` |
| Disband | the treasury | the cycle | `sink` intent | `Sink.City.Disband` |

Each tax arrives as a claim that the script redeems in its handler and records in `Journal`. If the WSH stays
down for more than 15 min, the reconciler refunds the claim to its payer (05 §1.23), so conservation holds and
the city only loses that revenue (counter `city.tax_refunded`).

**Projection and enforcement across cells and zones.** Every change to a city's status, rank, radius, zoning,
rates, civic stage, mayor or bans writes the keyed world flag `zone.<zone>.City.<id>` (05 §1.21). Its blob is
a `CityFootprint` of ≤ 512 B: centre, radius, rank, status, zoning, civic stage, rates, mayor and a 256 B Bloom
filter of banned characters. The write carries the partition's `(region, lease_gen)` and `expect_version =
footprintVer`, so a superseded WSH cannot overwrite a newer footprint.
- Every cell hosting any region of the zone watches `zone.<zone>.City.*`. At a tick boundary it rebuilds a
  256 m grid index of footprints, which feeds `PlacementCheck`, the city-tax lookup of purchases and fees,
  and the city terminals.
- The zone's `WorldFlags` singleton replicates the same footprints to clients, so the placement ghost uses
  identical data.
- The upkeep worker reads the same projection to find each structure's city and rates, and records the
  footprint version it used in the settle transaction.
- A city lies wholly inside one zone (rule 2), but its governance works from every zone, because residence,
  votes, taxes and views all go through the script.

**Terminals.** `TerminalDef.services` gains `Structure` (ACLs, pool deposits, residence, pack-up, *Pay debt*)
and `City` (08 §1.7.2's City panel: view, donate, stand, vote, and the mayor's rates, zoning, permits, bans and
withdrawals). City services call `World.call("CityGovernance.<Method>", req)` from the cell (§11), which
validates the live `TerminalSession` and the actor, and carries the acting character.

**Failures.** With the WSH down, cities keep their last footprint, so rates, unzoned placement and zoning
checks against the Bloom filter keep working. Zoned placement, residence changes, votes and withdrawals fail
with `CITY_UNAVAILABLE`, and tax claims wait up to 15 min. The kill switch `ws.CityGovernance` does the same on
purpose.

**Scale (Phase 4 shard).** ≤ 2,000 cities, ≤ 250k citizens, ≤ 60 invocations/s (tax claims dominate) and
≤ 40 MiB of rows, well inside 05 §1.23's quotas.

### 9.3 Territory control and sovereignty (G08; R09-A9, R01-Q14)

A territory structure is a `StructureDef` of kind `Territory` with a `StructureLifecycleDef`: an outpost, a
sovereignty hub, a faction base or a GCW-style base. The hosting cell simulates the fight, but the World State
service owns the lifecycle state (05 §1.21). Every state change is a version-CAS transition on its
`territory_structure` row, journaled in `territory_audit`, so it happens exactly once across cell kills.

```
record StructureLifecycleDef @table("slc") {
  anchorSecs: u32 = 900; layers: u8 = 3;                       // shield, armour, hull
  window: { hoursPerWeek: u8 = 21; minBlockHours: u8 = 3; changeDelayH: u16 = 96; };
  reinforceHours: list<u16> = [24, 48];                         // layers − 1 entries; exit snapped into a window
  finalVulnMins: u16 = 60;
  capture: { ability: AbilityRef; secsAtFull: u32 = 600; rangeM: f32 = 25_000; occupancyMult: CurveRef; };
  onFinal: enum { Destroy, Capture }; fightProfile: ZoneProfileRef;   // PreProvision size (04 §3.4)
  influence: list<InfluenceGrant>;                              // per hour while Online
}
```

| State | Entered by | Left by | Timer |
|---|---|---|---|
| Anchoring | placement (§9.2.1) | `anchorSecs` pass → Online; destroyed if its layer is depleted first | `terr:<s>:anchor` |
| Online | anchoring; a Vulnerable stage that ends without depletion | during an open window, layer k is depleted by damage or a capture channel completes → Reinforced(k) | window open and close |
| Reinforced(k) | layer k depleted | `reinforceUntil` → Vulnerable(k) | `terr:<s>:reinf:<k>` |
| Vulnerable(k) | the reinforce timer | defenders complete a capture reversal, or `finalVulnMins` pass with no depletion → Online (layers regenerate); attackers deplete layer k+1 → Reinforced(k+1), or the last layer → Destroyed or Captured | `terr:<s>:vuln:<k>` |
| Destroyed / Captured | the last layer | final. Destroyed: a killmail, and §9.2.3's reclamation transaction moves the contents into a wreck with loot rights (§8.7) and vacates the plot. Captured: the owner changes in the same transition | — |

- **Vulnerability windows.** The owner picks `hoursPerWeek` hours in blocks of ≥ `minBlockHours`, within
  `ZoneRulesDef` bounds (for example, at least 1 h in every 24 h). A change applies `changeDelayH` later and is
  refused while any layer is reinforced. Outside a window the structure carries `Structure.Invulnerable`:
  weapons still hit (`CanHarm`, §9.4), but damage stops at the current layer's floor, and capture channels fail
  with `NOT_VULNERABLE`.
- **Reinforcement exit.** `reinforceUntil` is the first window start at or after `now + reinforceHours[k]`,
  plus an offset in [0, 1 h) seeded by `SipHash(shardSalt, structure, k)` and stored in the audit row. The fight
  lands in the defender's chosen hours and cannot be predicted to the minute.
- **Capture** is an entosis-style `Channel` ability (§9.6, §1.4) on a target that is in a window or
  Vulnerable. Progress `p ∈ [0, 1]` advances by `1 / (secsAtFull × occupancyMult(index))` per second of **zone
  time**, so TiDi slows it with the fight. An opposing channel pauses progress, and a defender channelling
  alone reverses it at the same rate. Warp, cloak, range beyond `rangeM` or a `Capture.Break` tag stops a
  channel on that tick. The cell evaluates progress natively at 1 Hz and checkpoints it with its region every
  5 s, so a cell kill loses at most that window; `p = 1` is a transition request, never a local change.
- **Transitions.** The hosting cell requests damage- and capture-driven transitions with
  `Territory.Transition(structure, from, version, to, cause, evidence, idem)`, carrying its `(region,
  lease_gen)`. Timer-driven ones come from 05 §1.8 timers that the service consumes itself. Each is one PG
  transaction: the CAS on `version`, the audit row, the next timers and the outbox event. The new state reaches
  cells through KV `WORLD` (`zone.<zone>.Territory.<id>`) and applies at a tick boundary.
- **Pre-provisioning (R01-Q14).** The transaction that enters Reinforced(k) also writes an outbox request
  `PreProvision(zone, reinforceUntil, fightProfile)` to the orchestrator (04 §6.7), idempotent on
  `(structure, k, cycle)`. `reinforceUntil` is ≥ 24 h away, so the hint always exceeds the default 30 min lead.
  The orchestrator moves the zone onto a reserved host of that profile's class and releases it 2 h after the
  window.
- **Influence (SWG GCW, Elite BGS).** `InfluenceDef{faction, region, halfLifeH, thresholds: list<{flag, up,
  down}>}`. Cells batch `AddInfluence` every 10 s from kills, missions, events and Online territory
  structures (05 §1.21). The service decays values lazily in fixed point. It sets a control flag when a
  faction's share reaches `up` (default 0.60) and clears it only below `down` (0.50), so a region oscillating
  between the two never flips. Every flip is an audited world-flag write, read by spawners, vendors,
  `ZoneRulesDef` security and the Event Director.
- **Sovereignty (Phase 4).** A `SovHubDef` territory structure gives its owner `sov_state(system)`.
  - `SovUpgradeDef{power, workforce, fuelPerWeek, effects}` installs region modifiers: spawn sets, ore
    anomalies, jump bridges, jammers. Installs and removals are audited transactions that refuse
    `Σ power > Region.Power` or `Σ workforce > Region.Workforce`.
  - A weekly durable-timer job burns each hub's fuel (ledger items in its fuel bay, `Sink.Sov.Fuel`) exactly
    once, through the hub's owning cell or, for a parked hub, with the dormant precondition, as §9.2.3's
    reclamation does. Unfuelled upgrades go offline on that tick.
  - The occupancy index (0–5, from the owner's activity in the system, recomputed daily) drives
    `occupancyMult` from 1× to 4×, so a well-used system takes up to four times longer to capture.
- **Audit.** `territory_audit` holds every transition, window change, influence flip and upgrade, each with its
  cause and evidence (attackers, damage totals, channel holders, the reinforcement seed). An hourly audit folds
  each structure's rows and compares the result with its current state.
- **Scale (Phase 4 shard).** ≤ 20k territory structures and ≤ 100 transitions per minute.

### 9.4 PvP and law (R09-A16)

The native `CanHarm(attacker, target, ctx)` gates every hostile ability, effect, packet and hostile
interaction verb (§9.6). It checks `ZoneRulesDef` security, flag tags (`PvP.Overt`, `PvP.Faction.*`,
`Crime.Suspect`, `Crime.Criminal`), standings, wars, duels and groups; for followers it evaluates the
owner. Crimewatch timers (weapons, suspect, criminal, logoff) are timed tags, security status is a
persisted attribute, and NPC police are spawners triggered by `Crime.Criminal`.

### 9.5 Mounts (R02 §6.6, R02-Q17)

A `ControlDeviceDef` item references a vehicle or creature; §7.3's companions and pets use the same
device. *Call* spawns an owned entity in the owner's AG; *store* despawns it and saves its health, fuel
and loadout into the device payload. The movement, seats, `Drive` channel, prediction and call rules are
§8.1a's, and the server validates speed with §8.1a's caps (Phase 2).
- **Vehicle deeds (SWG).** A crafted or looted vehicle is a `ControlDeviceDef` item whose record template
  carries a `VehicleDef`. Its health, fuel and installed items (ledger location `(deviceItem, flag = port)`,
  §8.4) persist in the device. Repair uses a `Hold` verb (§9.6) at a garage interactable or with a repair
  kit, costing a ledger-debited resource per health point.
- **Creature mounts (SWG).** A pet (§7.3) whose record template carries a `VehicleDef` with `model: Mount`
  can be ridden once a `Pet.Trained.Mount` grant is on its device. Training is a creature-handler ability,
  and the grant is progression on the device item (05 §1.5). A ridden pet keeps its follower brain, which
  resumes on dismount.
- **Limits.** One called vehicle or mount per character. It counts against `Follower.Max.Pet` only when it
  is a pet. By default `call.zones` and `ZoneRulesDef` tags keep vehicles out of stations, interiors and
  space, and a forbidden kind is auto-stored on zone transfer, like followers.

### 9.6 Interaction framework (R09 P1 doors and airlocks, R02-Q8)

Every "use" in the world goes through one `Interactable` component bound to an `InteractableDef`:
doors, terminals, loot, seats, harvest nodes, NPC talk and hack panels.

```
record InteractableDef @table("ixn") {
  verbs: list<VerbDef>; highlight: enum { Outline, Glow, None } = Outline;
  client { promptAnchor: SocketName; }
}
struct VerbDef {
  verb: TagRef;                  // Interact.Use/Open/Loot/Talk/Sit/Board/Hack/Harvest/Repair/Scan
  prompt: LocString; requirement: Requirement; hostile: bool = false;   // hostile ⇒ CanHarm (§9.4)
  range: f32 = 2.5; requiresLOS: bool = true; facingDeg: f32?;
  mode: variant { Press; Hold{secs: Magnitude; cancelOn: TagSet; maxMove: f32 = 0.5};
                  Channel{ability: AbilityRef}; MultiUser{required: u8; windowSecs: f32} };
  maxUsers: u8 = 1; cooldownSecs: Magnitude?;
  action: variant { Ability(AbilityRef); Script(ScriptRef); Terminal(TerminalRef);
                    Container(LootRightsRef); Door(DoorRef); Seat(SeatRef); Dialogue(DialogueRef);
                    Emit(TagRef) };
}
```

- **Client focus.** A 10 Hz query covers a 5 m sphere (25 m in vehicles) within a 35° half-angle cone
  around the crosshair or cursor ray. Candidates score `angle/35° + distance/range`. The best one
  shows its prompt, and a key cycles through the alternatives. 08 draws the prompt as
  `InteractPrompt{verb, label, inputGlyph, state: Ready | Disabled(reason) | InProgress(pct),
  holdSecs}`. Disabled reasons use only the requirement's `client{}` part; server-only requirements
  show "Unavailable".
- **Server validation.** `RequestInteract{entity, verb, inputSeq}` is rate-limited to 10/s. The cell
  checks, in order:
  1. distance from the actor's capsule to the anchor ≤ `range` + 0.5 m + min(1.0 m, v·RTT/2);
  2. one LOS ray from eye to anchor against static, grid and dynamic layers, excluding actor and
     target;
  3. facing;
  4. the requirement (tags, attributes, items, `Perm.Check`);
  5. `CanHarm` for hostile verbs;
  6. locks, `maxUsers` and cooldown.

  Then it runs the action. A rejection returns a reason code, which the prompt shows.
- **Hold and channel.** The server timer runs on zone ticks from the accept tick while the client
  predicts the progress bar. Moving more than `maxMove`, taking damage tagged in `cancelOn`, or leaving
  range cancels it. `Channel` uses the ability pipeline, which is how hacking minigames work.
- **Multi-user.** `MultiUser` fires once, when `required` distinct users hold within `windowSecs`:
  twin-key terminals, heavy doors, reactor restarts. The single server event is keyed by the window's
  first tick.
- **Doors and airlocks.** `DoorDef{portal: PortalId?, openSecs = 1.2, autoCloseSecs = 8, lock:
  Requirement, powered: bool, airlock: AirlockSpec?}` replicates the state `Closed | Opening | Open |
  Closing | Locked | Unpowered`.
  - A door bound to a portal toggles the `PortalGraph` portal (02 §5.1), so 03's culling, audio
    occlusion, nav (§7.1) and pressure all follow one state.
  - Unpowered doors (the resource network, §8.5) can be forced with a `Hold` verb that needs a tool
    tag.
- **Pressure (the "06 pressure gameplay" of 02 §5.1).** Each pressurized interior cell has a
  `pressure` in [0, 1], evaluated at 2 Hz per interior.
  - Hull breaches (§8.7) and portals open to vacuum drain connected cells, with flow ∝ portal area ×
    Δp.
  - Occupants of a cell below 0.3 get `Env.Vacuum` (suffocation) unless they are `Env.Sealed` (helmet
    or suit), and loose physicalized items vent.
  - Airlocks cycle in `AirlockSpec.cycleSecs` (6 s by default) with interlocked doors.
- **Terminals.** `TerminalDef{ui: UiDocRef, services: list<enum{Market, Missions, Vendor, Bank, Mail,
  Crafting, Fitting, Customization, CrewMissions, Structure, City}>, diegetic: bool}`. `Structure` and
  `City` are §9.2's structure and city services.
  - Interacting opens a server `TerminalSession{actor, terminal, openedTick}`, which stays valid
    while the actor is within range + 2 m. §4's terminal-access check for market and vendor calls is
    this session.
  - Diegetic terminals render through 08 §1.8's `UiSurface`.
- **Loot containers.**
  - Contents are reward tokens (NPC loot, 04 §6.1) or ledger items (persistent containers and player
    wrecks).
  - `LootRightsDef` is `Owner`, `Party` (round-robin, or need/greed with a 30 s roll window), or
    `FreeForAll` after N seconds.
  - Pickup is `Grant(reward_token)`, keyed `(token, looter)`, into the looter's custody. When looters
    race, exactly one succeeds and the others see "already taken".
  - Locked containers use a `Hold` or `Channel` hack verb.
- **Events.** A successful interaction emits `Interacted{actor, target, verb}` into §6.1's event index,
  which feeds `Interact` objectives, codex entries and achievements.
- **Authoring.** Interactables are placed with prefabs in T01 and T02, and their verbs are edited in
  the T08 inspector. The viewport draws range, cone and anchor gizmos. T28 flags anchors with no
  navmesh point within range and LOS.

### 9.7 Emotes, moods and performances (R02-Q16, R05-P1-18)

```
record EmoteDef @table("emt") {
  command: Name; aliases: list<LocString>;              // "/wave" plus localized aliases
  montage: map<BodyTypeRef, AnimRef>; layer: enum { Full, Upper, Face }; loop: bool;
  posture: TagQuery?; cancelOnMove: bool = true;
  text: { untargeted: LocString; targeted: LocString }?;   // "{actor} waves at {target}."
  paired: { partner: map<BodyTypeRef, AnimRef>; align: SocketName; consent: bool = true }?;
  unlock: Requirement?;                                 // collection (§5.2) or store entitlement
  chatTriggers: list<string>;                           // ":)" or "lol" → Face/Upper layer
}
```

- **Play.** The client sends `PlayEmote{emote, target?}`, limited to 1/s. The cell validates the
  unlock, posture and state (no full-body loops in combat). It replicates `EmoteState{emote, target,
  startTick}` on the actor to its relevance set at the social-hub rate (R05-P1-18), and the montage
  plays through 02 §7.2's montage slot.
- **Emote text.** Each viewer renders the text from the localized template, within 30 m and aware of
  the portal graph. Emote text never passes through the chat service and is localized per viewer.
- **Paired emotes** (handshake, hug, partner dance). The requester asks and the target accepts within
  10 s. The cell then moves the requester to the target's align socket (a correction of at most
  1.5 m), and both states share one `startTick`.
- **Moods (SWG).** A `MoodDef` swaps the idle animation set and the chat verb ("says angrily").
  Chat triggers are matched on the sender's client against its own outgoing chat, and the server sees
  only `PlayEmote`.
- **Performances (SWG entertainers).** A `PerformanceDef` is a channel ability that loops emote
  montages, synced to a band leader's `startTick` and beat.
  - Watchers within 20 m for at least 30 s gain a `persist` buff effect.
  - Performers gain `Social.Dance` or `Social.Music` XP.
  - Healing battle fatigue is an effect removal.
- **Authoring.** Montages are authored in T15, records in T08 and T09, and text in T25.

### 9.8 Map data (for 08's Maps panel)

06 supplies the map data and 08 draws it.
- **Markers.** A `MapMarkerDef{icon, category, scope: Zone | Body | System | Galaxy, visibility:
  TagQuery over viewer facts, source: Static | QuestObjective | Party | Poi | Service}`.
  - Static and POI markers are cooked per zone.
  - Quest markers come from active objectives: their `Goto` volumes and targets.
  - Party members come from the party service.
  - Markers are phase-filtered like entities (§6.5).
- **Sensor honesty.** The minimap and radar show only entities already in the client's interest set.
  Hostile contacts appear only when the §8.5 sensor tick or §7.1 perception marks them as detected by
  the viewer or the viewer's party, so the map is never a wallhack.
- **Surface maps.**
  - The client generates each body's map from `engine/pcg` at a coarse tile level (height, biome ID,
    water). Generation is deterministic, so nothing is downloaded.
  - Authored `MapLayerDef` overlays add roads, settlements and region names.
  - Fog of war uses `MapRegionDef`s: at most 1,024 authored or biome-derived regions per body, stored
    as a Character-scope discovery collection (§5.2) of ≤ 128 B per body.
- **Interior maps.**
  - Interior maps are cooked from each interior container's `PortalGraph`: cell hulls are projected
    per deck into floor plans.
  - An `InteriorMapDef` maps cell IDs to room names and deck indices. Unmapped cells get generated
    labels and a T28 warning.
  - Door states and pressure hazards (§9.6) overlay live.
- **Space.** The system orrery and galaxy map (08) read frames and the jump graph. The local tactical
  map reads the sensor contact list.

### 9.9 Weather gameplay (03 §5.8a)

- **State.** 06 owns the replicated per-body `PlanetWeather{seed, epoch, fronts[≤ 32]{centre, radius,
  velocity, type, intensity, lightningRate}, baseWind, temperatureBias}` that 03 renders.
  - It is a pure function of `(bodySeed, epoch k, WeatherTableDef set, active overrides)`, evaluated
    with `det::`.
  - Epoch k = ⌊shard wall time / 600 s⌋. Front motion also uses shard wall time.
  - Every cell that hosts the body's surface or orbit computes the same value, so no weather owner
    has to fail over. Cells replicate it reliably when it changes (≤ 1 KB).
- **Front spawner.** At each epoch the spawner draws fronts from T18's per-biome `WeatherTableDef`s,
  using a PCG64 stream seeded by `hash(bodySeed, k)`. The tables hold weighted front types, intensity
  ranges and calendar seasonal modifiers. Fronts live 1–6 epochs and move along latitude wind bands.
  Clients extrapolate their positions analytically.
- **Overrides.** Scripted storms from the Event Director or a GM set `WeatherOverride` world flags
  (05 §1.21). An override enters the function at the next epoch boundary, or at once with a 60 s
  blend.
- **Gameplay effects.** The cell samples each character and vehicle at 1 Hz. Intensity is the
  maximum over fronts of `intensity·smoothstep(1, 0.6, d/radius)`.
  - A `WeatherEffectDef{frontType, minIntensity, effects, exemptIf: TagQuery}` applies timed effects:
    visibility (`ctx.visibility` for `detect()` and perception), sandstorm or acid damage, and cold.
  - `Env.Sheltered` exempts an entity. It applies inside a PortalGraph cell flagged roofed or
    pressurized, or under an overhead ray hit within 10 m. `Env.Sealed` also exempts.
  - Base and front wind feed §8.2's aero surfaces.
  - A strike from 03's deterministic strike schedule that lands within 10 m of an exposed entity
    resolves as a `Shape` damage packet.
  - Budget: ≤ 0.1 ms per tick for 500 players.

## 10. Character creation and species

- **Species.** A `SpeciesDef` holds the skeleton, retarget map, base-attribute effects, allowed
  parameters and equipment (`TagQuery`), body-type equipment variants, voice and animation sets,
  name rules and start locations (R09-A18). A species can be gated behind a legacy unlock (§5.2).
- **Parameters.** Each `CustomizationParamDef` is `{kind: Morph|BoneScale|PaletteColor|TextureLayer|
  Decal|MeshOption|DnaBlend, range, default, bits, speciesMask}`.
- **Appearance.** `CharacterAppearance` is a quantized vector of about 200–600 B: a header
  (species, body type, schema version) plus packed parameters. It is persisted by the character
  service and replicated once to observers. The server validates ranges, so no invisible or giant
  avatars. DNA blending (SC) stores up to four head-basis weights. Image-designer abilities edit
  another player's vector with consent.
- **Restyle and liveries** (the `Customization` terminal service of §9.6; 08 §1.7.2's Customization panel).
  - `Restyle{vector, quoteVersion}` is valid only inside a `TerminalSession`. It may change any parameter
    that the `SpeciesDef` allows, except species and body type. It gets the same range checks as creation,
    and the `unlock` checks of each option (collection, legacy or entitlement, §5.2).
  - The cell quotes a price per changed parameter group. It charges the quote as one ledger sink
    (`Sink.Customization.Restyle`, idempotent by intent) and applies the new vector at a tick boundary.
  - `SetLivery{ship, livery}` writes an unlocked livery into an owned hull's per-instance livery data
    (03 §4.2). It costs nothing, because a livery is paid for when it is unlocked.

## 11. Scripting API, graphs and determinism

**Luau hosting (ADR-002).**
- **VMs and sandbox.** One VM per zone per cell process runs on the zone's serialized *script
  lane*. The client and editor have their own VMs. Each module gets its own environment with frozen
  globals and no `io`, `os` or `debug`. Each VM has a memory quota. On cells the interrupt callback
  meters deterministic fuel at safepoints, and every engine binding call is charged calibrated fuel too. It **never yields**, so scripts yield only at visible
  calls (`wait`, `await*`, `task.yield`, `task.checkpoint`). Past ≈ 2 ms of fuel a resume is flagged
  and `task.checkpoint()` yields. At `fuel_kill` (≈ 5 ms) the resume is killed deterministically,
  and a 20 ms wall backstop is logged, so replays stay bit-exact (02 §7.4, 04 §10.2).
- **Coroutines are tasks.** `wait(zoneSecs)`, `awaitService` and `awaitEvent` block only the
  coroutine (the EVE tasklet lesson, R01-Q4). Coroutines never cross processes. When an entity's
  authority moves by handoff, planned migration or crash recovery, the new owner raises
  `Authority.Adopted{entity, cause}` for it, and the module re-arms from `ScriptState` (rule 7), where
  the results of re-issued service calls also arrive (04 §6.7); a cell's replay-keyframe rebase raises it with `cause = rebase` (04 §10.2). The analyzer flags a module that
  waits more than 5 s of zone time with no `Adopted` handler.
- **Capability manifests.** Each module declares one
  (`caps = {"items.grant", "wallet.faucet:Faucet.Quest.*"}`), checked at publish time. Calls that
  create value need a declared reason code and are capped per call and per hour. UGC (Phase 5)
  runs in a separate metered VM with no faucet or `progress.account` capabilities (R01-Q19).
- **Hot reload** runs a `__reload(old)` migration hook. ASM programs swap between activations, so
  in-flight instances finish on their own program hash.

**API surface** (typed Luau definitions generated by schemac):

| Namespace | Surface |
|---|---|
| `Entity`, `World` | Handles, whitelisted components, `WorldPos` f64 userdata (never a float `vector`), queries, raycasts, flags; `World.call` and `World.emit` to world scripts such as `CityGovernance` (05 §1.23, §9.2.4) |
| `Attr`, `Tags`, `Effects`, `Ability` | Read; apply and remove effects by record; grant; `tryActivate` for server-only abilities |
| `Items`, `Wallet`, `Market` | **Intents only**, returning futures: `Items.Grant(char, def, qty, reason, idemKey)` |
| `Quest`, `Dialogue`, `Event`, `Phase`, `Cinematic` | Stage control, sessions, director hooks |
| `Activity`, `Encounter`, `Match` | Read roster, tier and modifiers; `Encounter.SetPhase`, `Encounter.Wipe`; `Match.Score(team, points, reason)`, which appends to the event log (§6.11) and never sets a score directly |
| `Spawn`, `AI`, `Timer` | Spawners, blackboards, squad orders; zone-time and **durable** timers |
| `Follower`, `Interact`, `Progress` | Summon, store and command owned NPCs; interactable state, locks and custom verbs; account-progress reads and `Progress.Report(criterion, n)` (capability `progress.account`) |
| `Rand`, `Cue`, `Net` | Named seeded streams (`math.random` is removed); cues; schema'd, rate-limited client RPC |
| `UI`, `Input` (client) | Data-bound UI models; intents to the server |

**Graph families** (ADR-002) and what each compiles to:
- ability → ASM or Luau;
- formula → HXL;
- quest, event, mission, encounter and match rules → data state machines with Luau actions;
- dialogue → records;
- BT and utility (including follower brains) → flat arrays and curves;
- spawner, progression (skills, achievements, collections, season tracks), loot and plug sets →
  records, with EV simulators;
- level logic and minigames → Luau;
- cues → client records.

**Determinism and replication rules (normative):**
1. Predicted logic is ASM or native only. It runs on command frames, uses `hmath`, and draws RNG
   keyed by `(entity, predictionKey, frame)`. Jolt's broadphase order is nondeterministic, so
   query results are sorted by entity ID before use.
2. Luau never runs in the prediction path or in hot loops.
3. Server randomness comes only from named seeded streams. Value-bearing rolls use salted-hash
   seeds and audits.
4. Gameplay time is dilatable zone time. Wall time is used only for durable timers, `Wall` effects,
   calendar resets, and the weather epoch and front motion (§9.9), which must agree across zones.
5. Value moves only through synchronous ledger calls with idempotency keys and epochs.
6. Cross-authority-group effects are messages. Handles are re-validated after every yield. Yields
   happen only at explicit calls, and a stale handle raises `StaleHandle` rather than touching
   another entity (02 §7.4).
7. Replicated state is declared in `.hschema` with an audience. Scripts add it only through
   schema'd `ScriptState` components.
8. Clients send only schema'd, rate-limited intents that are never trusted for outcomes.
9. Server scripts are replayable. They never iterate tables keyed by tables, userdata or functions
   (the order is address-dependent); entity-keyed maps use `EntityMap`, which iterates in EntityId
   order. The `simdet` lint enforces this (04 §10.2).
10. HXL follows §1.2's cross-language float rules in both interpreters.

## 12. Feature ladder, acceptance, risks and traceability

### 12.1 MVP → AAA ladder

| System | P0 Foundations | P1 First Light | P2 Alpha Sandbox | P3 Beta Scale | P4 Launch Quality | P5 Ambition |
|---|---|---|---|---|---|---|
| Kernel | Tags, attributes, modifiers, HXL C++/Go + corpus (FMA-sensitive vectors, `hxlfloat` lint) | Effects, abilities, **predicted ASM**, cues | Cross-entity modifiers, brain snapshot, fitsim | Correct under handoff | Budgets met, WASM fitsim | UGC-safe API |
| Items | ItemDef schema | Starter loadout granted through the core ledger (no trading) | **Ledger inventory**, containers, sockets/rolls, loot, fitting | Decoration, org hangars | GM restore, plug APIs | Physicalized cargo |
| Crafting | — | — | Resources, survey, harvesters, schematics, industry jobs | Factories, invention | Minigames, balance tools | — |
| Economy | Reason-code registry | — | Wallets, vendors, market, contracts, trade, taxes, telemetry | Player vendors, city taxes through the `CityGovernance` treasury (§9.2.4) | Insurance, economy guard at scale | Economy sim |
| Progression | — | XP and levels | Graphs, standings, titles, migrations; **achievements** | Faction ranks; **collections, codex, legacy**, entitlement mirror | Power rules; **seasons and reward tracks** | — |
| Owned NPCs | — | — | **Companions** (gear, influence, stances), pets and taming, drones, command channel, crew missions | Multi-cell handoff, NPC crew in seats, carrier fighters, drones at fleet scale | Companion conversations auto-staged (T13) | — |
| Quests / events | — | One scripted encounter | Quests, missions, generator, dialogue, world flags | **Group conversations**, phasing, public events | Meta-events, auto-staging hooks | UGC missions |
| Activities / PvP | — | — | — | **Activity lifecycle, encounters, checkpoints, `ClaimGuard` lockouts, tiers and modifiers, sync and bolster, solo mode, roster policy; match rules, rated 6v6, risk zones; queues, group finder, leaderboards** | Ranked seasons, leaderboard windows, 16-player operations | Cross-shard queue federation |
| AI | — | BT, perception, navmesh | Utility, spawners, space steering, threat | Squads/fleets, interior nav | **LOD T0–T3**, HTN | Ecosystem sim |
| Flight / combat | — | FBW flight with the **bit-exact allocator**, basic quantum travel, hitscan, simple respawn | Command flight, master modes, sensors/EWAR, all strategies, seats; **ground vehicles and mounts** (wheeled, hover and mount models, `Drive` channel, owner prediction; §8.1a, GP-4d) | Boarding, **EVA** (suit thrusters, magboots, grabs, tethers, gravity switch), shield faces, killmails; **player fleets** (fleet warp, broadcasts, command bursts, fleet killmails; §8.3a); tracked and motorcycle drivetrains, mounted combat, vehicle turrets, hangar drive-out | Resource network, device aim assist; fleets at 2,000-ship scale (NS-4.2) | Destruction hooks; articulated vehicles (trailers, walkers) |
| Interaction / social | — | `Press` verbs, doors, terminal sessions, marker data | Hold and multi-user verbs, loot containers and rights, emotes, surface and interior maps | Paired emotes, performances, airlocks and pressure | — | — |
| World / social | — | — | Guild permissions, mounts, PvP flags | **Housing** (ledger plots and lots, `PlacementCheck`, decoration caps, the upkeep chain), **cities** (`CityGovernance`), **territory** (windows, capture, reinforcement with `PreProvision` hints, influence), crimewatch; weather gameplay (GP-16, GP-17) | **Sovereignty** hubs, upgrades and fuel (GP-17 e) | Player-programmable structures |
| Character | — | Species + basic morphs | Full parameter set | Image-designer edits | DNA blend | — |

### 12.2 Acceptance criteria (each is an automated CI test or benchmark; phases per 09 §3.2 #15)

Other sections cite item *n* as **GP-*n***. Phases for the items added in round 1: GP-10 Ph2 (zone
transfer, kill, budget, commands, crew missions) and Ph3 (cell-boundary handoff, fleet-scale drones);
GP-11 Ph2 (achievements, a–c), Ph3 (collections, codex, legacy, e) and Ph4 (seasons, d); GP-12 Ph1
(`Press` verbs, doors, terminals: a, first half of d) and Ph2 (the rest); GP-13 Ph3. Round 2: GP-4 is
split into 4a (FBW, including the bit-exact clause) Ph1, 4b (command flight) Ph2 and 4c (EVA) Ph3; GP-14
(a)–(f) Ph3, (g) Ph4 and (h) Ph5. Round 3: GP-15 (player fleets) Ph3 at 500 ships and Ph4 inside NS-4.2's
2,000-ship battle; GP-4d (ground vehicles and mounts) Ph2, with the tracked and motorcycle layouts joining its
hash in Ph3. Round 4: GP-16 (housing and cities) Ph3, with (a)–(c) and (e) in WP-3.6, (d) in WP-3.12 (which
builds the world-script host that `CityGovernance` runs on) and (f) in WP-3.2; GP-17 (territory) Ph3, with
(a)–(d) in WP-3.6, and (e) sovereignty Ph4 in WP-4.5.

1. **Kernel.** 200 golden ship and character builds match reference values within 1e-9, and are
   bit-identical on MSVC, GCC and Clang. C++ and Go HXL agree on 100% of the corpus, including the
   ≥ 1,000 FMA-sensitive vectors, on `windows/amd64`, `linux/amd64` at `GOAMD64=v1` and `v3`, and
   `linux/arm64`. The `hxlfloat` lint reports zero findings and catches 100% of its planted
   violations. A 300-modifier ship recompute takes ≤ 50 µs; an incremental change takes ≤ 5 µs; 10k
   entities × 40 attributes at 5% dirty resolve in ≤ 1 ms on 8 workers.
2. **Ability prediction round-trip** (150 ms RTT, 2% loss, 60 Hz, 50 bots). Local response comes
   in the input's frame. Confirmation arrives within RTT + 2 ticks at p95. Mispredictions stay
   under 1%. A 15-frame rollback takes ≤ 0.5 ms. Forged or replayed keys are rejected.
3. **AI.** 5,000 NPCs per cell (≥ 400 in T0) tick in ≤ 4 ms of wall time per 20 Hz tick on 8
   workers. 1,000 NPCs all in T0 combat take ≤ 8 ms. 2,000 path queries/s at p99 ≤ 5 ms. Paths on
   a moving, rotating ship are correct.
4. **Flight, EVA and ground vehicles.**
    - (a) *FBW (Ph1).* Thruster allocation takes ≤ 20 µs per ship. 200 FBW ships at 30 Hz take ≤ 4 ms
      per tick. Own-ship prediction error stays under 5 cm at 100 ms RTT. **Bit-exact clause:** 64 ships
      over 6 hull layouts (4–48 thrusters) run 3,600 steps at 30 Hz on recorded and fuzzed inputs. The
      inputs include full-stick saturation on all six axes, coupled and decoupled flight, SCM and NAV
      switches, 12 scripted thruster damage, destruction and repair events, cargo mass changes,
      centre-of-mass shifts, and atmospheric flight with scheduled wind. The per-step hash of `(u_cmd,
      u_actual, w, PID state, body state)` is bit-identical on MSVC (VS 2022 and VS 2026), clang-cl, GCC,
      Clang and MinGW, at 1, 4 and 16 workers, and between the cell path and the client
      `SingleBodyPredictor` path. From Ph3 the suit thruster sets of §8.2a join the same run. In a 30-min
      contact-free soak of 50 bot pilots at 100 ms RTT and 1 % loss, corrections tagged `divergence` are
      0. It runs in RT-03's CI job.
    - (b) *Command flight (Ph2).* 3,000 command-flight ships at the 2 Hz fleet-battle tick take ≤ 10 ms
      per tick.
    - (c) *EVA (Ph3).* 1,000 scripted BENCH-6 EVA runs per night at 100 ms RTT and 1 % loss. Each run
      has two segments. First, with both ships coasting at 300 m/s, a character leaves a *Kestrel*'s
      exterior shell, crosses 40 m of open space and enters the *Mule*'s shell. Second, with the *Mule*
      under 1 g thrust, the character walks 20 m on magboots over a 90° hull edge, grabs a handhold,
      tethers, cycles the airlock and enters the 1 g interior, starting the transfer upside down relative
      to grid up. Results:
      - mispredicted ticks are < 1 % of EVA ticks, counting every transfer and gravity switch;
      - re-orientation completes within 0.5 s, and the character stands on the interior floor;
      - there are 0 fall-throughs (RT-11), 0 `physics.eva_fallthrough_guard` hits and 0 flagged
        transfers (04 §5.5);
      - fuel exhaustion stops thrust on the same tick on client and cell, and O2 exhaustion applies
        `Env.Vacuum` within 1 tick.
    - (d) *Ground vehicles and mounts (Ph2; §8.1a).*
      - **Hash.** 48 vehicles over 6 layouts run 3,600 steps at 60 Hz on recorded and fuzzed `Drive` inputs.
        The layouts are a 2-pad speeder bike, a 4-pad landspeeder, a 4-wheel rover with an automatic
        gearbox, a 6-wheel truck with a manual gearbox and two differentials, a quadruped mount and a biped
        mount; a tracked vehicle and a motorcycle join in Ph3. The inputs include full throttle, brake and
        steer saturation, handbrake turns, shifts in both gearbox modes, boost to exhaustion, hovering over
        cliff edges and water, tilt- and slope-limit hits, casts on seams between collision tiles, gait
        changes, stamina exhaustion, jumps, mounting and dismounting, and 12 scripted wheel, pad and engine
        damage events. The per-step hash of the chassis state, the constraint's `SaveState` bytes, and the
        hover, boost, fuel and gait state is bit-identical on MSVC (VS 2022 and VS 2026), clang-cl, GCC,
        Clang and MinGW, at 1, 4 and 16 workers, and between the cell path and the client
        `SingleBodyPredictor` path (the character mover for mounts). It runs in RT-03's job.
      - **Soak.** 100 bot speeders and 20 bot rovers drive Harrow's mountain and plains presets for 30 min on
        the ground-hub profile (20 Hz ticks, physics at 60 Hz through §8.1a's 3 collision steps), at 100 ms
        RTT and 1 % loss. 80 speeders, dispersed ≥ 1 km apart, roam random waypoints at up to 100 m/s, and
        20 ride in convoys of 5 at 15 m spacing. 30 bot riders on the two mount types share the zone. A
        mispredicted tick is one on which the client rewinds. Results:
        mispredicted ticks are < 1 % of owned-vehicle ticks and of mounted ticks, counting every cause;
        corrections tagged `terrain` or `divergence` are 0; a probe finds 0 vehicles below the collision
        surface; `physics.collision_tile_miss` = 0; mounting and dismounting land on the predicted tick with
        0 fall-throughs; and vehicle stepping takes ≤ 1 ms p99 of stage 3's wall time on 8 workers.
      - **60 Hz activity.** A 32-rider swoop race in an Activity-profile instance ticking at 60 Hz meets the
        same misprediction and correction thresholds, and two replays of the race give identical lap times.
5. **Economy conservation.** In a 72-hour soak of 5,000 bots with chaos kills of cells and
   services, the hourly audit shows zero deviation for every currency and item definition
   (Σ created − Σ destroyed = Σ held). Replayed idempotency keys and stale-epoch writes produce no
   duplicates.
6. **RNG.** Every plug set, loot table and resource roll passes chi-square (p > 0.01) over 10⁶
   samples, plus the perk-pair independence test. Audited rolls reproduce exactly.
7. **Durability.** Jobs, harvesters, maintenance and reinforcement timers fire exactly once across
   restarts. Quests and events resume within 5 s after a cell kill, with no double rewards. GP-16 (c) and
   GP-17 (c) test the upkeep and reinforcement chains end to end.
8. **Group conversation.** Four players resolve within RTT + 100 ms of the last choice or the
   timeout. A disconnect never blocks the session.
9. **Scripting and quests.** A runaway script dies within 5 ms without a tick overrun. Everything
   hot-reloads in ≤ 2 s. A cell holds 5,000 concurrent quest instances, and dispatch cost does not
   grow with quest count.
10. **Owned NPCs.**
    - (a) *Survival.* A bot owner has a companion carrying 12 gear items, 3 pets and 5 launched
      drones. It makes 1,000 zone transfers (Ph2), then 1,000 cell-boundary handoffs (Ph3), and its
      owning cell takes 100 `kill -9`s.
      - Every summoned follower simulates on the owner's first authoritative tick, restored from the
        same checkpoint `(epoch, seq)`, or the same replicant state (04 §6.4, Ph4), as the owner.
      - The hourly ledger audit shows zero duplicated, lost or re-custodied gear, drone or device
        items.
      - Influence XP is never applied twice and loses at most the owner's checkpoint window.
    - (b) *Abandonment.* Warping out with drones outside the bay creates exactly one abandoned drone
      per drone item, with 0 duplicates across 100 chaos kills.
    - (c) *Budget.* Both of these cells meet GP-3's 4 ms at 20 Hz on 8 workers, with follower brains
      adding ≤ 1.5 ms:
      - a 500-player hub cell with 500 summoned companions (20% in combat) and 2,000 ambient NPCs;
      - an open-space cell with 300 ships, 1,500 launched drones (60% engaged) and 1,000 NPCs.
    - (d) *Commands.* The p95 time from command intent to the follower's first state change is ≤
      RTT/2 + 1 tick. Commands from a wrong owner, or to out-of-range drones, are rejected 100% of the
      time.
    - (e) *Crew missions.* 10k concurrent crew missions across 20 Industry-service restarts each
      deliver exactly once, and the delivered items match the dispatch-time roll audit.
11. **Account progression.**
    - (a) 10k bots advance 200 counters each for 24 h with cell and service chaos kills.
      - Every completed node is granted exactly once.
      - Counter loss is at most one 30 s flush window, or the owner's state-loss window
        (AAA-SRV-9) on a cell kill.
      - No counter ever exceeds the true count.
    - (b) Event-dispatch cost with 5,000 defined achievements is within ±10% of the cost with 50.
    - (c) Account rows stay ≤ 32 KiB per account. `ApplyAccountProgression` has p99 ≤ 20 ms at 2k
      flushes/s.
    - (d) *Seasons (Ph4).* `calendar.season.end` locks the track on every cell within 60 s.
      Claim-window expiry mails unclaimed rewards exactly once. Premium ranks never grant without the
      entitlement.
    - (e) A mirrored unlock appears on a second shard within 60 s.
12. **Interaction, emotes and maps.**
    - (a) *Forged requests.* 10k forged `RequestInteract`s are 100% rejected: out of range beyond
      tolerance, through walls, locked, wrong verb, or replayed. 10k legitimate ones at 150 ms RTT
      are ≥ 99.9% accepted. 500 players at 2 interactions/s cost ≤ 0.3 ms per tick.
    - (b) *Hold and multi-user.* A hold completes within ±1 tick of `holdSecs`, and a two-user verb
      fires exactly once.
    - (c) *Loot races.* 50 looters racing for one container produce exactly one grant per token.
    - (d) *Doors and pressure.* A door change reaches the portal graph (culling, audio, nav and
      pressure) within 1 tick on the cell. A breach decompresses a 10-cell interior to within 5% of
      the reference curve.
    - (e) *Emotes.* 500 hub players emoting at 1/s cost ≤ 2 kbit/s per client, and paired emotes
      start on the same tick for both actors.
    - (f) *Maps.* Harrow's surface map generates on the client in ≤ 2 s on MIN. Every interior
      container cooks a map with zero unlabeled-cell errors. In 10k randomized trials, no undetected
      hostile ever appears on the minimap.
13. **Weather.**
    - Every cell hosting Harrow's surface or orbit produces a bit-identical `PlanetWeather` hash for
      10k random epochs on MSVC, GCC and Clang.
    - Weather sampling for 500 players takes ≤ 0.1 ms per tick.
    - Sheltered or sealed entities never receive exposure effects in 10k randomized placements.
14. **Activities, matches and matchmaking** (§6.6–6.13; 05 §1.12).
    - (a) *Matchmaking (Ph3).* One shard holds 20k queued players at steady state across five queues: a
      3-player roleless strike, a 4-player flashpoint (1 tank, 1 healer, 2 damage), an 8-player operation
      (2/2/4), and unrated and rated 6v6. Solo and party tickets (sizes 2–4) arrive by a Poisson process.
      Each queue's role mix is within ±10 % of demand, and 30 % of solo tickets offer two roles.
      - Players are matched at p95 ≤ 60 s and p99 ≤ 180 s overall, and each role's p95 is ≤ 90 s.
      - There are 0 role-minimum violations, 0 split parties and 0 players in two matches.
      - ≥ 90 % of rated matches have a predicted win probability within 0.5 ± 0.1.
      - A 5k-ticket bucket pass takes ≤ 100 ms of CPU. A matchmaker leader kill loses no ticket
        (rebuilt from the mirror in ≤ 5 s).
      - T12's queue simulator, fed the same arrivals, predicts the p95 within ±10 %.
    - (b) *Lockouts (Ph3).* 10k bots run lockout-scoped encounters for 24 h, across a weekly reset, with 100
      cell `kill -9`s, 20 activity-service and ledger restarts, and duplicated completion reports. The
      audit finds exactly one `ledger_guard` row per (subject, key, period) and exactly one grant per
      guard: 0 duplicate lockout rewards and 0 grants without a guard. Every cell applies the reset
      within 60 s.
    - (c) *Checkpoint resume (Ph3).* A cell killed mid-encounter in 200 BENCH-4 *Hollow Vault* runs is
      replaced, and the instance resumes in ≤ 5 s. Completed encounters and instance flags are kept, the
      engaged encounter restarts from its last phase checkpoint, and completion rewards are never paid
      twice.
    - (d) *PvP match (Ph3).* 1,000 6v6 Control matches at 60 Hz, with bots at 150 ms RTT and 2 % loss. The
      final score equals the offline fold over the event log in 100 % of matches. Scripted mercy,
      overtime and same-tick tie cases resolve as specified. Every result is reported once and every
      rating updated once, despite duplicated reports and a cell kill in 50 matches. The cell tick p99
      stays within the 60 Hz profile budget.
    - (e) *Tiers and sync (Ph3).* For 200 golden builds, synced and bolstered characters land within ±2 %
      of the tier's target stats, bolster never exceeds the target, and `perPlayer` scaling is frozen at
      encounter start in 100 % of join and leave fuzz cases.
    - (f) *Roster policy (Ph3).* Scripted idlers are flagged AFK within `secs` + 5 s, with 0 false
      positives over 1,000 active-bot runs. Backfill fills a slot at p95 ≤ 90 s under (a)'s load and
      never admits a player while `State.InEncounter`. Vote-kicks respect quorum, immunity and cooldown in
      100 % of fuzz cases.
    - (g) *Ranked seasons and leaderboards (Ph4).* A rated season rolls over on `calendar.season.end` on
      every cell within 60 s. Leaderboards take 10k writes/s per shard with top-100 reads at p95 ≤ 50 ms
      and "around me" at p95 ≤ 100 ms, and after a Valkey loss they are rebuilt from PG with 0 lost
      entries.
    - (h) *Cross-shard federation (Ph5).* Two federated shards match (a)'s load with the same targets, and
      the conservation audit of each shard shows 0 value crossing shards.
15. **Player fleets** (§8.3a; 05 §1.12.1). Ph3 runs it in a 2 Hz fleet-battle zone with 500 ships; Ph4 runs it
    inside NS-4.2's 2,000-ship battle.
    - (a) *Fleet warp.* A 250-member bot fleet (5 wings × 5 squads × 10) waits at a staging point ≥ 1 AU from a
      battle grid in the same zone, where 250 other ships fight (Ph4: 1,750), with `d` forced to 0.1. The FC
      fleet-warps it to a beacon at 30 km, 100 times, with hulls mixed so that warp speeds and align times
      differ by ≥ 3×. Pass: in 100 % of warps every eligible ship lands on the same tick; each lands in its
      formation slot, bit-exact in fixed point on the cell and on every observing client; no two ships land
      closer than the slot spacing; scrambled and out-of-range members are refused with a reason and never
      move; the tick p99 stays within the 2 Hz profile budget and `d` ≥ 0.1 through the warp-in.
    - (b) *Broadcasts.* 10k broadcasts of every kind across 20 fleets reach exactly their declared audience
      (0 misses, 0 deliveries to non-members or to the wrong level) at p95 ≤ 1 tick + RTT from the intent's
      arrival, at `d` = 1 and at `d` = 0.1. Rate-limited excess is dropped at ≤ 0.1 ms of tick time.
    - (c) *Command bursts.* 1,000 randomized bursts each apply to exactly the fleet members within range at the
      activation tick, against an offline geometry oracle (0 misses, 0 extras). Per kind only the strongest
      applies, refreshes and expiries land on their specified ticks, and 50 golden boosted builds match their
      reference attributes within 1e-9.
    - (d) *Roster and attribution.* A 256-member fleet forms from invites and an advert in ≤ 60 s at 1 % loss.
      Every roster change reaches voice, chat, cells and clients at p99 ≤ 1 s, and a move with a stale
      `roster_ver` is rejected. Killmails from (a)'s battle carry each attacker's `fleetId`, and each
      killmail's fleet damage shares sum to 1 ± 1e-6.
16. **Housing and cities** (§9.2; Ph3). Run on a two-cell Harrow surface zone and a second zone, on Windows
    and Linux.
    - (a) *Placement parity.* 10k fuzzed placements cover Harrow's plains and mountain presets, Saltmarch's
      no-build polygons, 40 `StructureDef`s, cube-face and zone edges, the region seam, and positions in and
      around 20 projected cities of all five ranks (GM-written fixture footprints until (d) lands). For each,
      every determinism toolchain (ADR-001a rule 7) runs `PlacementCheck` on the same input snapshot: the
      client builds (both MSVC toolsets, the floor being the release toolset, and clang-cl), the cell builds
      (GCC and Clang) and MinGW. The verdict, reason and `samplesHash` agree in 100 % of cases. In a live run, bots at 150 ms RTT make 10k placements: every
      client–cell disagreement is explained by a manifest or footprint version difference (0 unexplained), and
      an offline geometric oracle finds 0 accepted placements that break a rule.
    - (b) *Caps under concurrency.* 1,000 rounds. In each, 32 bots holding 10 lots each fire placements that
      overlap one another from both cells at the same tick, including plots that straddle the seam, while
      ownership transfers and pack-ups run; 16 admins drop decoration into one structure at once. Results: no
      `Lot` balance is ever negative, an offline check of `ledger_plot` finds 0 overlapping plots, no structure
      exceeds `itemCap`, no zone exceeds `maxStructures`, and every refusal carries a reason code.
    - (c) *Upkeep chain.* 20k structures, with pools sized to run dry at random times, go through arrears,
      three decay stages, condemnation and reclamation on compressed timings (60 s stages, 10 min
      condemnation). The run includes 50 cell `kill -9`s, 20 Industry-service and 10 ledger restarts, and
      duplicated timer deliveries. Each settle, stage change, condemnation and reclamation happens exactly once
      (the fold of `structure_upkeep` matches the journal). Each reclamation moves every interior item into
      exactly one reclaim container (0 lost, 0 duplicated) and releases its plot and lots exactly once. Paying
      the debt in any decay stage, or while condemned, restores the structure with exactly one charge. The
      hourly conservation audit shows 0 deltas for credits, lots and items.
    - (d) *City governance* (WP-3.12). 200 founding attempts race for 50 centres in one zone; 5,000 bot
      citizens declare, move and lose residences across both zones; cycles are compressed to 5 min; 50
      elections run. The run includes 20 WSH `kill -9`s and 10 `worldscript`-service and ledger restarts.
      Results:
      - no two cities are closer than `minCenterSpacingM`, and failed founders' reservations expire;
      - live `Citizen` rows and `Residence` rows form a bijection after quiescence, and with
        `oneCitizenPerAccount` no account is a citizen twice;
      - each city's rank follows the threshold table, with the two-cycle downgrade rule, in 100 % of cycles,
        and founding deadlines disband exactly the cities below the Outpost threshold;
      - each election settles exactly once, its winner equals the offline tally (ties per §9.2.4), and no
        voter has two ballots;
      - every flow in §9.2.4's tax table reaches the treasury under its reason code; the escrow-backing audit
        matches every hour; claims left unredeemed during a > 15 min WSH outage are refunded exactly once; 0
        conservation deltas;
      - each footprint change reaches every cell of the zone and every client within 5 s, and a cell's
        placement verdicts follow the new footprint on the tick it applies it.
    - (e) *BENCH-5 through the rules.* A scripted bot builder places BENCH-5's 300 structures and 3,000
      decoration items in Saltmarch through `RequestInteract` → `PlacementCheck` → the ledger. The ledger's plot
      rows and the zone manifest match the scene 1:1, with 0 structures placed by editor fiat, and this state is
      BENCH-5's world snapshot. From WP-3.12 the builder also founds the Saltmarch city through
      `CityGovernance`, and 30 bot citizens take it to rank 4 (City) before the snapshot is taken.
    - (f) *Housing instances* (WP-3.2). 1,000 housing interiors park on `idle_ttl` and reload 10 times each
      with decoration transforms bit-exact. (c)'s chain also runs on parked structures through the dormant
      precondition, with the same exactly-once results.
17. **Territory and sovereignty** (§9.3; Ph3, sovereignty Ph4). Every clause ends with the audit: for each
    structure, the fold of `territory_audit` equals its current state (0 mismatches), and every transition names
    its cause and evidence.
    - (a) *Windows.* 500 structures with random owner windows over all 168 hours of the week, on a simulated
      wall clock that runs a week in 3 h. 100k scripted damage and capture attempts outside a window cause 0
      layer loss below the floor and 0 capture progress. Window changes apply exactly `changeDelayH` later and
      are refused while a layer is reinforced.
    - (b) *Capture channel.* 1,000 capture races at 100 ms RTT, with contested pauses, reversals and broken
      channels (warp, cloak, range, `Capture.Break`). Completion time equals the offline formula within one
      tick of zone time, at `d` = 1 and at `d` = 0.1 (TiDi). A broken channel stops progress on the same tick.
      50 cell kills mid-capture lose at most 5 s of progress and never commit a transition twice.
    - (c) *Reinforcement and pre-provisioning.* 1,000 reinforcements run on compressed timers
      (`reinforceHours` = 1 h), with 20 World State service restarts. Every entry into Reinforced issues exactly
      one `PreProvision` request, with `time = reinforceUntil`, at least 30 min before it. Every
      `reinforceUntil` falls inside an owner window, and its offset reproduces from the audited seed. ≥ 95 % of
      zones are on their reserved host by `time − 5 min` (04 §6.7), and the rest raise `PreProvisionMissed`.
    - (d) *Influence.* A 24 h replay of recorded and fuzzed `AddInfluence` batches, with duplicated and
      reordered batches, produces the same control-flag sequence as the offline fixed-point fold. A series
      oscillating between `down` and `up` flips 0 times.
    - (e) *Sovereignty (Ph4).* 200 hubs under install and remove fuzz never exceed their region's power or
      workforce. Each weekly fuel burn happens exactly once across 20 service restarts, unfuelled upgrades go
      offline on the burn tick, and capture times follow the occupancy multiplier within one tick.

### 12.3 Risks and mitigations

| Risk | Mitigation |
|---|---|
| Kernel cost explodes under EVE-style fits | Incremental aggregation, dependency graph, slot constants, benchmarks |
| Cross-compiler float divergence (mispredicts, desync) | Strict FP, `hmath`, Jolt determinism, desync telemetry; a fixed FBW allocation order and quantized inputs, hashed on every toolchain (§8.2, GP-4a); corrections tagged by cause, with `divergence` paging |
| Scarce roles make queues unbounded (DPS glut) | Role minimums never relax; fill bonuses for the short role; multi-role tickets; per-role p95 in GP-14a; T12's queue simulator before a queue ships |
| Lockout rewards duplicated across instances, retries or cell kills | `ClaimGuard` guard rows committed with the grant in one ledger transaction (§6.8), audited in GP-14b |
| Ground vehicles mispredict on terrain, or Jolt's vehicle code diverges between toolchains or between cell and predictor | One chassis body per vehicle, with the constraint's `SaveState` in the rollback snapshot; cast ties broken by `TileKey` or entity ID, never `BodyID`; 3 collision steps per 20 Hz tick; a state hash in every owner snapshot with a full-state correction; GP-4d's hash in RT-03's job; `terrain` and `divergence` corrections page (§8.1a) |
| EVA falls through floors or snags at the gravity switch | Sphere shape during re-orientation, capsule shape casts before standing, placement validated against portal cells, a guard counter that must stay 0 (§8.2a, GP-4c) |
| Go fuses multiply-adds (arm64, `GOAMD64=v3`), so Go HXL drifts from C++ | §1.2 rules: explicit `float64()` rounding, no constant arithmetic, `hxlfloat` lint, pinned `GOAMD64=v1` checked at start, FMA-sensitive corpus on v1, v3 and arm64 |
| Ability graphs become spaghetti (R08 F2) | Domain node set, 64-state cap, text-diffable source, native escape hatch |
| Scripts inflate the economy or stall ticks | Capability manifests, reason-code caps, interrupt budgets, kill switches |
| Dupes at seams (New World); biased RNG (Destiny) | Synchronous ledger, idempotency, fencing, chaos tests, audits; salted seeds, statistical CI |
| Followers duplicate gear or strand at seams | Followers ride the owner's AG, with no fence row of their own; gear is ledger items in the owner's custody; launched drones stay in the bay ledger-side; GP-10 chaos tests |
| One companion per player doubles hub AI cost | Follower LOD (T1 escort unless in combat or commanded), counted in GP-3's budget, GP-10 hub test |
| Achievement counters become a write storm | Counters in the AG checkpoint; 30 s batched delta flushes; completions only as immediate ledger grants |
| Persistence bloat (SC PES, SWG sprawl) | Lifetime policies, caps, maintenance reclamation, cargo deferred to Phase 5 |
| Concurrent placements overlap across a region seam, or overspend lots or item caps | The ledger's plot exclusion constraint arbitrates; `Lot` is a ledger currency with `balance ≥ 0`; the owning cell is the single writer of an interior; GP-16 (b) |
| The upkeep chain double-charges, strands items or skips a stage after a crash | One ledger transaction per settle and per reclamation, each with a guard; the row's `seq` and next timer commit together; parked structures take the dormant path; GP-16 (c, f) |
| City governance has no owner, or alts and crashes rig it | The `CityGovernance` world script owns it with actor partitions, inbox dedup and escrow-backed treasuries; one residence per account; elections guarded by `status`; GP-16 (d) |
| Territory timers are gamed (window flipping, timezone tanking) or lost in a crash | Window changes take `changeDelayH` and are frozen while reinforced; exits snap into the owner's window with a seeded offset; every transition is a CAS on the World State service with an audit row; GP-17 |
| Live rebalancing breaks players (NGE) | Versioned progression with migrations, feature flags, staged publishing |

### 12.4 Traceability

| Requirement | Section |
|---|---|
| R09-A0, R09-A14, R09-G1, R01-Q7, R01 §5 | §1.1–1.3, §1.6 |
| R05-P0-3, R05-P1-12, R05 §4.2 | §1.4, §11 |
| R09-A15, R09-G7, R05-P0-8, R05-P1-15, R02-Q6 | §2 |
| R09-A7, R02-Q9, R02-Q10, R01-Q13 | §3 |
| R09-A6, R09-A8, R09-G8, R01-Q8, R02-Q11, R04-P2-20 | §4 |
| R02-Q13, R02 §6.1, R05 §5.1; R05-P1-16 and 01 G17 (seasons); SWTOR legacy and codex, Destiny collections and triumphs | §5 |
| R09-A12, R09-G12, R03-P0-6, R03-P0-11, R03-P2-2, R05-P1-17 | §6.1–6.5 |
| 01 G12 and G19; R03-P1-8 (tiers, level sync and bolster, solo and scaled modes, group finder; cross-shard in Ph5); R05-P0-5/6 (activity host, placement); R05-P1-14 (systemic modifiers); R05-P1-15 (weekly lockouts); R05-P2-22 (opt-in PvP risk zones with extraction) | §6.6–6.13 |
| R09-A13, R09-G9, R02-Q14, R05-P1-14 | §7.1–7.2 |
| 01 G11 (companions), R03-P1-7, R02-Q17 (pets, control devices), EVE drones and fighters | §7.3 |
| R09-A1–A5, R09-A17, R09-G3–G6, R09-G11, R04-P1-13–15, R05-P0-4, R05-P0-9, R03-P1-10 | §8 |
| 01 M09 (EVA, boarding), BENCH-6, 04 §5.3 (bit-exact hull prediction) | §8.2, §8.2a, §8.4 |
| 01 M03 (on foot in Ph1; vehicles and mounts in Ph2), R02 §6.6 and R02-Q17 (vehicle deeds, creature mounts), 04 §5.3 (ground-vehicle prediction) | §8.1, §8.1a, §9.5, GP-4d |
| 01 M01; R01 (fleet warp-ins, fleet boosts, fleet hierarchy); 04 §2.7 fleet voice | §8.3a (with 05 §1.12.1), GP-15 |
| R09-A9–A11, R09-A16, R09-G10, R02-Q12, R02-Q17, R03-P1-9, R01-Q14 | §9.1–9.5 |
| 01 G07 (housing, decoration, player cities; SWG-critical), R02 §6.4 and lesson 5 (placement, lots, maintenance, condemnation, cities with ranks, taxes and elections) | §9.2, GP-16 |
| 01 G08 (territory, timers; EVE-critical), R09-A9 (vulnerability windows, entosis capture, sovereignty upgrades on power and workforce, GCW influence), R01-Q14 (timers pre-provision capacity) | §9.3, GP-17 |
| R09 P1 (doors, airlocks), R02-Q8 (portal cells), R02-Q16 and R05-P1-18 (social, emotes), 01 W08, 03 §5.8a (weather) | §9.6–9.9 |
| R09-A18 | §10 |
| R01-Q4, R01-Q19, R03 §8.1, R04-P2-23 | §11 |

**Dependencies on other sections.**
- **02-engine-runtime:** the `.hschema` grammar (`client{}`/`server{}`, `@store`), tag and HXL
  cook steps, `hmath` with strict FP, grids and `Reparent()`, Luau budgets, terrain modifications,
  the `PortalGraph` (interior maps, doors, pressure) and montage slots (emotes); zero-gravity `Exterior`
  host grids for EVA shells (§8.2a); Jolt `VehicleConstraint` and hover step listeners, 3 collision steps in
  bubbles that hold a vehicle, and `SingleBodyPredictor` support for a constraint's `SaveState` (§8.1a,
  WP-2.2); and RT-03's CI job, which also runs GP-4a's FBW hash and, from Ph2, GP-4d's vehicle hash.
- **03-rendering:** renders `PlanetWeather`, lightning and wind emitters (§1.5, §9.9); this section
  answers 03 §9.6's round-1 ask.
- **04-networking:** command frames, the owner channel with prediction keys, a ≥ 500 ms hitbox
  history, phase filtering, and cross-group messages and handoff payloads. Followers are members of
  their owner's AG with no fence row of their own (04 §6.1 note). 04 §5.3's bit-exact hull prediction
  rests on §8.2's determinism rules and GP-4a. The `Flight` channel's six axes and throttle travel in
  the §5.2 input record, and EVA characters are predicted in the owning grid's coordinates (§8.2a). The
  `Drive` channel takes `Flight`'s place in the input record for ground vehicles and mounts, and 04 §5.3
  predicts them under §8.1a's contract.
  Activity instances are 04 §7 instances.
- **05-backend:** aligned with its §1.6–1.15, plus these additions, all specified: a Go HXL package
  (`pkg/hxl`, 05 §8, with §1.2's float rules); a world-state flags service (05 §1.21, which also owns
  the influence and sovereignty accumulators of §9.3 and `WeatherOverride` flags); an Economy Sim
  service (05 §1.22); escrow refund timers for crafting sessions and crew-mission jobs (05 §1.8);
  account progression storage and `ApplyAccountProgression` (05 §1.5); entitlement mirroring
  (05 §1.20); and, for §6.6–6.13, the matchmaker, group finder, leaderboards, `LockoutView`,
  `ReportActivityResult`/`ReportMatchResult` and cross-shard federation (05 §1.12), plus the ledger's
  `ClaimGuard` op (05 §1.6). For §9.2–9.3: the ledger's plot ops and `ledger_plot` constraint (05 §1.6), the
  structure upkeep worker (05 §1.8), keyed city footprints and the territory rows and transitions (05 §1.21),
  and system-job escrow claims into world scripts, with `CityGovernance` as a Foundation world script
  (05 §1.23).
- **07-editor:** the graph compilers (T09, T12, T13), the AI and spawn tool, EV simulators, and the
  "view as state" debugger. T23 and T13 author followers and influence, and T09 authors account
  progression and season tracks. T12's Activity mode authors §6.6–6.13 (encounter graphs, lockouts,
  tiers, match rules, queues) and runs the queue simulator. T01 authors EVA exterior shells (07 §2.6.4).
  T21 assembles vehicle chassis and drivetrains with the envelope preview, T15 authors mount and rider
  animation sets and extracts gait speeds, and T28 runs `phys.vehicle.*` (§8.1a).
- **08-client:** interaction prompts on the HUD; the Maps panel (surface, interior, minimap); the
  Progression panel (achievements, collections, codex, legacy, season track); the emote list and
  wheel; the Activities panel (director, queue and ready check, group finder, scoreboard, leaderboards;
  §6.6–6.13); EVA suit gauges on the HUD (§8.2a); the `Vehicle` input context for `Drive`, and vehicle and
  mount gauges on the HUD (speed, gear, boost, fuel, stamina; §8.1a); the placement ghost running
  `PlacementCheck` over the `ZoneStructures` manifest and city footprints, and the Structure and City panels
  bound to `CityGovernance` (§9.2).
