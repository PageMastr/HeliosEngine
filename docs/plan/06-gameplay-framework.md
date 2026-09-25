# 06 — Gameplay Framework

> **Status:** draft v1. **Conforms to:** ADR-002, -004, -006, -007, -008 and -012.
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

**Modifiers.** A modifier is `{attr, op, magnitude, penaltyGroup, exempt, requirement: TagQuery?, source}`.
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
  `{domain: Self|Owner|Pilot|Ship|ShipModules|Fleet|Grid|AreaVolume, filter: TagQuery}`, so one
  skill can modify every module tagged `Item.Weapon.Hybrid`.
- **Brain snapshot (R01 §3.2).** A character's aggregated modifiers form a serialized
  `CharacterBrain` that travels with it on handoff instead of being recomputed.

**HXL (Helios eXpression Language).** HXL is pure, deterministic stack bytecode compiled by
schemac. Formulas are data, not C++ (R09-A0).
- **Built-ins:** arithmetic, `min max clamp lerp select`, `pow exp ln sqrt asinh` from the
  deterministic `hmath` library (not libm), `curve(t, x)`, `attr(e, A)`, `tag(e, T)`, `stacks()`,
  `level()`, and context fields.
- **Interpreters.** There are two, each about 1.5k lines: C++ and Go. A golden conformance corpus
  must produce bit-identical f64 results on MSVC, GCC, Clang and Go.

```
formula TurretHitChance(src, tgt, ctx) =                       // R09-A2, as data
  0.5 ^ ( (ctx.angularVelocity * 40000 / (attr(src,TrackingSpeed) * attr(tgt,SignatureRadius)))^2
        + (max(0, ctx.distance - attr(src,OptimalRange)) / attr(src,FalloffRange))^2 )
```

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
              resetPeriod: bool; onExpire: enum{ClearAll, RemoveOneAndRefresh}; overflow: EffectRef? };
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

All gating is native: requirements, costs, cooldowns, charges and the `CanHarm` check (§9). Only
the body varies. **Predicted** abilities compile from the ability graph to an immutable,
content-hashed **ASM** program (ADR-002, R05-P1-12):

```
struct AsmProgram  { AsmState states[]; AsmTransition transitions[]; AsmOp ops[]; VarLayout vars; };
struct AsmInstance { u64 programHash; u16 state; u16 flags; u32 stateEnterFrame;   // POD <= 192 B
                     u32 predictionKey; EntityHandle target; i32 vars[32]; };   // snapshotted per frame
```

Ops are fixed 16-byte instructions: `Wait(frames|inputRelease|event)`, `CommitCost`,
`StartCooldown`, `ApplyEffect`, `GrantTagTimed`, `ShapeQuery`, `TraceHitscan`,
`SpawnPredictedProjectile`, `SetMovementOverride`, `PlayMontage`, `TriggerCue`, `Branch` and
`EmitServerEvent`. `EmitServerEvent` is the only bridge to Luau, and it fires on the server after
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
  decals, camera shake, haptics and shield-impact ring-buffer writes (03-rendering). Servers never
  load cue payloads, and the cook enforces the split (R02-Q6).
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
  displaces the fewest items.
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
experimentation ceiling is `ceiling(q_p)`.

The **crafting session** is a server state machine:
`Open → Select → Slot → Assemble → Experiment×N → Customize → Finalize(Prototype | ManufacturingSchematic)`.
- **Slot** moves ingredients into escrow (`EscrowOpen`). A durable timer refunds them if the
  session dies.
- **Rolls** use seeded, audited streams.
- **Minigames** (Luau or graph) return bonuses that the cell clamps.
- **Finalize** is one atomic ledger transaction: consume the inputs, create the output stamped
  `CraftStamp{crafterId, crafterName, serial, station, time}`, and grant XP.

**Factories and industry.**
- **Factories** run manufacturing schematics as Industry-service jobs. Every unit copies the
  prototype's stats.
- **EVE industry.** `BlueprintDef` activities (manufacture, ME/TE research, copy, invention,
  reactions) declare inputs, outputs, `duration: HxlExpr`, facility bonuses and fees (a sink). The
  cell submits the job context. The Industry service recomputes duration and fee in Go HXL within
  clamps and registers a **durable timer**. Jobs progress offline and survive restarts (R01-Q13).

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
  character's station, system and region, validates terminal access, and attaches a **fee
  context**: a `Character.BrokerFeeRate` attribute from skills and standings, which the service
  clamps to the `TaxPolicyDef` bounds.
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

**One model.** SWG skill boxes, EVE trained skills, Destiny subclass nodes and talent trees are all
`ProgressionGraphDef`s (R02-Q13):
- **Graph contents.** Caps (such as `SkillPointCap = 250`) and nodes. Each node has prerequisites,
  costs (`Xp{type, n}`, `Points`, `Currency`, `TrainTime{HxlExpr}`), grants (permanent "skill
  mods" as `persist` effects, abilities, schematics, certification tags, titles, unlocks), an
  exclusive group and a refund policy.
- **XP.** `XpTypeDef` defines typed XP (`Combat.Rifle`, `Craft.Weapons`, `Social.Music`), with caps
  and rest bonus. Gameplay events grant it.
- **Time-trained skills** queue in the Industry/Timers service and progress offline.
- **Levels and power.** `LevelCurveDef` defines level curves. Destiny-style **Power** is a derived
  HXL attribute over equipped items. `ActivityDef.powerRules` applies a cap and a delta-scaling
  effect when a player enters an activity.
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

## 6. Missions, quests, dialogue and events

- **Quests.** The quest graph (T12) compiles to a **server state machine in data**, with Luau only
  for actions and custom predicates.
  - `QuestDef` holds stages, objectives, conditions (the Conditions/Facts system shared with
    dialogue, AI and spawns), rewards, failure, timers, sharing rules and phase actions.
  - Objective types: `Kill(TagQuery,n)`, `Collect`, `Goto(volume)`, `Interact`, `Escort`, `Scan`,
    `Craft`, `Deliver`, `Survive`, `Custom(Luau)`.
  - A `QuestInstance{quest, owner: player|party|world, stage, counters, vars}` moves with the
    character's authority group.
  - **Event index.** Gameplay events (`Killed`, `ItemAcquired`, `EnteredVolume`, `Crafted`…)
    dispatch through an index keyed by `(eventType, TagIndex)`. Only active objectives subscribe,
    so dispatch cost does not grow with the number of quests.
  - Rewards are ledger grants keyed by `(questInstance, stage)`, so they are never paid twice.
- **Mission generators (AO, EVE, SC).** A `MissionTemplateDef` holds parameter ranges, a site
  template (an object container or modular tiles, R05-P2-21), an objective blueprint,
  `reward: HxlExpr` (over difficulty, distance, standings and `RegionEconomyState`), availability
  and weights.
  Generators run on the cell per terminal or agent, with seeded offers refreshed on a timer.
  Accepting an offer spawns the site through the **site spawner**: anomaly entities with a
  lifetime, signature strength and template (R09-A5), found by the generic probe-scan query.
- **Dynamic and public events (GW2, Destiny).** An `EventDef` graph holds phases, objectives, a
  participant registry, **contribution scoring** (weighted damage, healing, interactions and
  presence) with reward tiers, participant-count scaling curves applied as effects, and
  success/fail branches that form chains.
  - A per-zone **Event Director** schedules events from the live-ops calendar, world flags and
    economy weights.
  - State is checkpointed to the activity store (`ACTIVITY` KV, 05 §1.12), so a replacement cell
    rehydrates within 5 s (R05-P0-5).
  - `WorldFlagDef`s (scoped to shard, zone or region; persisted; replicated) let results change
    spawner sets and vendor stock.
  - Meta-events that span zones coordinate through a world-state service over NATS.
- **Dialogue and group conversations (SWTOR, R03-P0-11).** `DialogueDef` nodes are
  `Line{speakerRole, lineId, stagingMeta}` (a stable `lineId` for localization and VO),
  `Choice{options{paraphrase, conditions, alignmentDelta, effects}}`, `Branch`, `Action` and
  `Cinematic`. A server `ConversationSession` holds eligible party members in range, role bindings
  (including a `PlayerSpeaker` slot), the current node, a server timeline, a choice window
  (default 30 s) and a resolver: `Roll`, `Leader`, `Vote` or `Owner`.
  1. Each player's own side effects (alignment, companion influence, facts) apply to their own
     choice immediately.
  2. **Roll:** each participant rolls d100 plus modifiers from a seeded, audited stream. The
     winner's option plays with the winner's character, voice and animation set.
  3. Social points are a typed XP: 1 per roll and 2 per win, scaled by group size.
  4. A line is skipped only when all participants skip it. A disconnected participant auto-passes
     after 5 s, so a session never blocks on one client.
- **Cinematic hooks.** `Cinematic.Play(seq, participants, blocking)` tags players
  `State.InCinematic` (immune, ignored by AI). Clients play the sequence synced to server time.
- **Phasing.** Actions call `Phase.Enter/Leave(layer)`. An entity's `PhaseFilter` is a TagQuery
  over viewer facts. The content cook assigns each distinct filter a bit of 04 §7's 64-bit per-zone
  `phase_mask`; the cell sets a viewer's bits when its facts change, and interest management tests
  the masks (R03-P0-6, 04 §4.3). The editor's
  "view as state" debugger uses the same evaluator.

## 7. AI

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
  connections (airlocks, ramps, elevators) trigger `Reparent()`. DetourCrowd is capped per grid.
- **Space AI.** Steering runs on top of the movement models (§8.1): seek, arrive, orbit and
  keep-at-range, align, warp-out. Formations are slots in the leader's frame. A **fleet/squad
  coordinator** entity uses utility scoring to issue primary targets, remote-repair assignments,
  EWAR distribution and retreat.
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

## 8. Vehicles, flight and combat

### 8.1 Movement models (R09-A1, R09-G3)

Each record template picks one model. The gameplay controllers sit on 02-engine-runtime's physics:
- `CharacterVirtual`: on foot, grid-local.
- `NewtonianFBW`: 6-DoF ships.
- `CommandKinematic`: EVE "ball" movement in 1 Hz zones; commands replicate, not transforms.
- `Hover`, `Wheeled` and `Mount`.
- `Travel`: warp and quantum travel.

### 8.2 6-DoF flight control (Elite, SC)

A `ThrusterDef` on an item port defines mount, direction, max thrust, spool time constant, power
and fuel draw, and health. Each tick the pilot's client predicts, and the cell authoritatively
runs, this pipeline:

1. **Input.** Stick, throttle and strafe, after device curves.
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
   `AeroSurfaceDef` lift and drag apply in atmosphere.

The kernel builds with strict FP (no fast-math, no FMA contraction) and uses `hmath`, which keeps
mispredictions rare across MSVC and GCC.

### 8.3 Command flight and long-range travel

- **Command flight.** `CommandKinematic` stores `{mode: Approach|Orbit|KeepAtRange|Align|Stop|Warp,
  target, range}`. It approaches max velocity exponentially: `t₇₅ = ln(4)·I·m/10⁶`.
  Clients integrate the same commands, with periodic corrections and desync telemetry (R01-Q10).
  In tactical zones the same commands are **autopilot behaviors** feeding `NewtonianFBW`, so
  "orbit at 5 km" works everywhere, for players and AI.
- **Warp and quantum travel.** A `TravelDriveDef` sets spool, alignment tolerance, minimum velocity
  fraction (EVE 0.75), speed profile, fuel, obstruction and interdiction. The cell computes a
  deterministic `s(t)`, replicated as `{from, to, t0, profile}`. Streaming prefetches along the
  path, interdiction effects can abort, and the entity reparents on arrival (R04-P1-15).

### 8.4 Multi-crew seats, stations and item ports

- **Item ports.** `ItemPortDef` hierarchies on vehicle and structure prefabs have a name, accepted
  `TagQuery`, size, transform and child ports. An installed item's ledger location is
  `(vehicleItem, flag = portId)`, so fitting, persistence and loot share one tree (R04-P1-14).
- **Seats and stations.** `SeatDef`/`StationDef` grant **input channels**: `Flight`,
  `TurretGroup[n]`, `Engineering`, `Scanner`, `Comms`. Enter and exit keep one continuous camera
  (ADR-010). Access is gated by owner, org crew roles or party ACLs. The cell routes each channel to
  a player or an AI brain, so AI crew press the same inputs. The pilot predicts the ship; a gunner
  predicts only the turret.

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
  distance, strength)`.
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
3. **Subsystem routing** sends a share to the component matching the hit-zone tag (R09-A2).
4. `DamageApplied` and `PoolDepleted` events feed cues, threat, contribution, quests and
   killmails.

Packets that cross authority groups arrive as `ApplyDamage` messages with their idempotency key.

**Death pipeline (R09-A17, R09-G11).** `DeathPipelineDef` is chosen per record template and
`ZoneRulesDef`. It runs: optional incapacitation with a revive window → death →
`onVehicleDestroyed: ejectTo` a capsule (EVE pod) → per-item drop, destroy or keep rolls (seeded,
audited, one ledger transaction) → a wreck entity with loot and a lifetime → a `Killmail` event →
respawn selection from `RespawnPointDef` candidates (bind points, clone bays, medbeds) → penalty
effects (wounds, implant loss) → an insurance claim paid on a durable timer.

## 9. Social and world systems

- **Guilds and corporations.** The Go social service owns membership and roles (05 §1.11).
  - Permission names (`Perm.Hangar.Div3.Take`, `Perm.Structure.Dock`) are declared in the schema and
    map to bits of its 128-bit masks.
  - Cells evaluate `Perm.Check(actor, resource, action)` against the `PERMS` KV cache, using ACLs
    that reference org roles, so every service shares one RBAC model (`pkg/perm`, R09-A11).
  - Org wallets and hangars are ledger owners.
- **Structures, housing and cities (R02-Q12, R03-P1-9).**
  - A `StructureDef` holds: footprint polygon, max slope and height delta, no-build radius, lot
    cost, region rule tags, terrain stamp (a modification layer), interior container, maintenance
    per period, decay stages, item cap and power needs.
  - **Placement validation** on the cell samples footprint heights for slope, queries no-build
    regions and neighbors' radii, checks the lot budget and zoning, charges through the ledger, then
    spawns the structure and persists and replicates its terrain modification (02/03 feed collision
    and LOD).
  - **Decoration** stores item transforms in the instance payload, within interior bounds and the
    item cap. ACLs cover admin, entry, ban and vendor rights.
  - **Maintenance** is a durable-timer sink. Unpaid structures decay, are condemned, then are
    reclaimed into a reclaim container (R02 §8.5).
  - **Cities.** A `CityDef` sets rank thresholds (citizens, radius), civic unlocks per rank, taxes,
    elections (Go) and a weekly durable job.
- **Territory control (R09-A9).** `StructureLifecycleDef` runs `Anchoring → Online → Reinforced →
  Vulnerable → Destroyed`, with owner-chosen vulnerability windows within bounds. Reinforcement
  timers are durable and emit **pre-provisioning hints** to the orchestrator (R01-Q14). Capture is
  an ability (an entosis-style channel). `InfluenceDef` accumulators per faction and region, with
  a decay half-life, set world flags for GCW-style control. Sovereignty upgrades consume region
  power and workforce attributes.
- **PvP and law (R09-A16).** The native `CanHarm(attacker, target, ctx)` gates every hostile
  ability, effect and packet. It checks `ZoneRulesDef` security, flag tags (`PvP.Overt`,
  `PvP.Faction.*`, `Crime.Suspect`, `Crime.Criminal`), standings, wars, duels and groups.
  Crimewatch timers (weapons, suspect, criminal, logoff) are timed tags, security status is a
  persisted attribute, and NPC police are spawners triggered by `Crime.Criminal`.
- **Mounts (R02 §6.6).** A `ControlDeviceDef` item references a vehicle or creature. *Call* spawns
  an owned entity; *store* despawns it and saves its health, fuel and loadout into the device
  payload. The server validates mounted speed.

## 10. Character creation and species

- **Species.** A `SpeciesDef` holds the skeleton, retarget map, base-attribute effects, allowed
  parameters and equipment (`TagQuery`), body-type equipment variants, voice and animation sets,
  name rules and start locations (R09-A18).
- **Parameters.** Each `CustomizationParamDef` is `{kind: Morph|BoneScale|PaletteColor|TextureLayer|
  Decal|MeshOption|DnaBlend, range, default, bits, speciesMask}`.
- **Appearance.** `CharacterAppearance` is a quantized vector of about 200–600 B: a header
  (species, body type, schema version) plus packed parameters. It is persisted by the character
  service and replicated once to observers. The server validates ranges, so no invisible or giant
  avatars. DNA blending (SC) stores up to four head-basis weights. Image-designer abilities edit
  another player's vector with consent.

## 11. Scripting API, graphs and determinism

**Luau hosting (ADR-002).**
- **VMs and sandbox.** One VM per zone per cell process runs on the zone's serialized *script
  lane*. The client and editor have their own VMs. Each module gets its own environment with frozen
  globals and no `io`, `os` or `debug`. Each VM has a memory quota. The interrupt callback yields
  at a 2 ms per-tick soft budget and kills at a 5 ms hard budget.
- **Coroutines are tasks.** `wait(zoneSecs)`, `awaitService` and `awaitEvent` block only the
  coroutine (the EVE tasklet lesson, R01-Q4).
- **Capability manifests.** Each module declares one
  (`caps = {"items.grant", "wallet.faucet:Faucet.Quest.*"}`), checked at publish time. Calls that
  create value need a declared reason code and are capped per call and per hour. UGC (Phase 5)
  runs in a separate metered VM with no faucet capabilities (R01-Q19).
- **Hot reload** runs a `__reload(old)` migration hook. ASM programs swap between activations, so
  in-flight instances finish on their own program hash.

**API surface** (typed Luau definitions generated by schemac):

| Namespace | Surface |
|---|---|
| `Entity`, `World` | Handles, whitelisted components, `WorldPos` f64 userdata (never a float `vector`), queries, raycasts, flags |
| `Attr`, `Tags`, `Effects`, `Ability` | Read; apply and remove effects by record; grant; `tryActivate` for server-only abilities |
| `Items`, `Wallet`, `Market` | **Intents only**, returning futures: `Items.Grant(char, def, qty, reason, idemKey)` |
| `Quest`, `Dialogue`, `Event`, `Phase`, `Cinematic` | Stage control, sessions, director hooks |
| `Spawn`, `AI`, `Timer` | Spawners, blackboards, squad orders; zone-time and **durable** timers |
| `Rand`, `Cue`, `Net` | Named seeded streams (`math.random` is removed); cues; schema'd, rate-limited client RPC |
| `UI`, `Input` (client) | Data-bound UI models; intents to the server |

**Graph families** (ADR-002) and what each compiles to:
- ability → ASM or Luau;
- formula → HXL;
- quest, event and mission → data state machines with Luau actions;
- dialogue → records;
- BT and utility → flat arrays and curves;
- spawner, progression, loot and plug sets → records, with EV simulators;
- level logic and minigames → Luau;
- cues → client records.

**Determinism and replication rules (normative):**
1. Predicted logic is ASM or native only. It runs on command frames, uses `hmath`, and draws RNG
   keyed by `(entity, predictionKey, frame)`. Jolt's broadphase order is nondeterministic, so
   query results are sorted by entity ID before use.
2. Luau never runs in the prediction path or in hot loops.
3. Server randomness comes only from named seeded streams. Value-bearing rolls use salted-hash
   seeds and audits.
4. Gameplay time is dilatable zone time. Wall time is used only for durable timers, `Wall` effects
   and calendar resets.
5. Value moves only through synchronous ledger calls with idempotency keys and epochs.
6. Cross-authority-group effects are messages. Handles are re-validated after every yield.
7. Replicated state is declared in `.hschema` with an audience. Scripts add it only through
   schema'd `ScriptState` components.
8. Clients send only schema'd, rate-limited intents that are never trusted for outcomes.

## 12. Feature ladder, acceptance, risks and traceability

### 12.1 MVP → AAA ladder

| System | P0 Foundations | P1 First Light | P2 Alpha Sandbox | P3 Beta Scale | P4 Launch Quality | P5 Ambition |
|---|---|---|---|---|---|---|
| Kernel | Tags, attributes, modifiers, HXL C++/Go + corpus | Effects, abilities, **predicted ASM**, cues | Cross-entity modifiers, brain snapshot, fitsim | Correct under handoff | Budgets met, WASM fitsim | UGC-safe API |
| Items | ItemDef schema | Starter loadout granted through the core ledger (no trading) | **Ledger inventory**, containers, sockets/rolls, loot, fitting | Decoration, org hangars | GM restore, plug APIs | Physicalized cargo |
| Crafting | — | — | Resources, survey, harvesters, schematics, industry jobs | Factories, invention | Minigames, balance tools | — |
| Economy | Reason-code registry | — | Wallets, vendors, market, contracts, trade, taxes, telemetry | Player vendors, city taxes | Insurance, economy guard at scale | Economy sim |
| Progression | — | XP and levels | Graphs, standings, titles, migrations | Faction ranks | Power rules, seasons | — |
| Quests / events | — | One scripted encounter | Quests, missions, generator, dialogue, world flags | **Group conversations**, phasing, public events | Meta-events, auto-staging hooks | UGC missions |
| AI | — | BT, perception, navmesh | Utility, spawners, space steering, threat | Squads/fleets, interior nav | **LOD T0–T3**, HTN | Ecosystem sim |
| Flight / combat | — | FBW flight, basic quantum travel, hitscan, simple respawn | Command flight, master modes, sensors/EWAR, all strategies, seats | Boarding, shield faces, killmails | Resource network, device aim assist | Destruction hooks |
| World / social | — | — | Guild permissions, mounts, PvP flags | Housing, cities, territory, crimewatch | Sovereignty, pre-provisioning | Player-programmable structures |
| Character | — | Species + basic morphs | Full parameter set | Image-designer edits | DNA blend | — |

### 12.2 Acceptance criteria (each is an automated CI test or benchmark; phases per 09 §3.2 #15)

1. **Kernel.** 200 golden ship and character builds match reference values within 1e-9, and are
   bit-identical on MSVC, GCC and Clang. C++ and Go HXL agree on 100% of the corpus. A
   300-modifier ship recompute takes ≤ 50 µs; an incremental change takes ≤ 5 µs; 10k entities ×
   40 attributes at 5% dirty resolve in ≤ 1 ms on 8 workers.
2. **Ability prediction round-trip** (150 ms RTT, 2% loss, 60 Hz, 50 bots). Local response comes
   in the input's frame. Confirmation arrives within RTT + 2 ticks at p95. Mispredictions stay
   under 1%. A 15-frame rollback takes ≤ 0.5 ms. Forged or replayed keys are rejected.
3. **AI.** 5,000 NPCs per cell (≥ 400 in T0) tick in ≤ 4 ms of wall time per 20 Hz tick on 8
   workers. 1,000 NPCs all in T0 combat take ≤ 8 ms. 2,000 path queries/s at p99 ≤ 5 ms. Paths on
   a moving, rotating ship are correct.
4. **Flight.** Thruster allocation takes ≤ 20 µs per ship. 200 FBW ships at 30 Hz take ≤ 4 ms per
   tick. 3,000 command-flight ships at 1 Hz take ≤ 10 ms. Own-ship prediction error stays under
   5 cm at 100 ms RTT.
5. **Economy conservation.** In a 72-hour soak of 5,000 bots with chaos kills of cells and
   services, the hourly audit shows zero deviation for every currency and item definition
   (Σ created − Σ destroyed = Σ held). Replayed idempotency keys and stale-epoch writes produce no
   duplicates.
6. **RNG.** Every plug set, loot table and resource roll passes chi-square (p > 0.01) over 10⁶
   samples, plus the perk-pair independence test. Audited rolls reproduce exactly.
7. **Durability.** Jobs, harvesters, maintenance and reinforcement timers fire exactly once across
   restarts. Quests and events resume within 5 s after a cell kill, with no double rewards.
8. **Group conversation.** Four players resolve within RTT + 100 ms of the last choice or the
   timeout. A disconnect never blocks the session.
9. **Scripting and quests.** A runaway script dies within 5 ms without a tick overrun. Everything
   hot-reloads in ≤ 2 s. A cell holds 5,000 concurrent quest instances, and dispatch cost does not
   grow with quest count.

### 12.3 Risks and mitigations

| Risk | Mitigation |
|---|---|
| Kernel cost explodes under EVE-style fits | Incremental aggregation, dependency graph, slot constants, benchmarks |
| Cross-compiler float divergence (mispredicts, desync) | Strict FP, `hmath`, Jolt determinism, desync telemetry |
| Ability graphs become spaghetti (R08 F2) | Domain node set, 64-state cap, text-diffable source, native escape hatch |
| Scripts inflate the economy or stall ticks | Capability manifests, reason-code caps, interrupt budgets, kill switches |
| Dupes at seams (New World); biased RNG (Destiny) | Synchronous ledger, idempotency, fencing, chaos tests, audits; salted seeds, statistical CI |
| Persistence bloat (SC PES, SWG sprawl) | Lifetime policies, caps, maintenance reclamation, cargo deferred to Phase 5 |
| Live rebalancing breaks players (NGE) | Versioned progression with migrations, feature flags, staged publishing |

### 12.4 Traceability

| Requirement | Section |
|---|---|
| R09-A0, R09-A14, R09-G1, R01-Q7, R01 §5 | §1.1–1.3, §1.6 |
| R05-P0-3, R05-P1-12, R05 §4.2 | §1.4, §11 |
| R09-A15, R09-G7, R05-P0-8, R05-P1-15, R02-Q6 | §2 |
| R09-A7, R02-Q9, R02-Q10, R01-Q13 | §3 |
| R09-A6, R09-A8, R09-G8, R01-Q8, R02-Q11, R04-P2-20 | §4 |
| R02-Q13, R02 §6.1, R05 §5.1 | §5 |
| R09-A12, R09-G12, R03-P0-6, R03-P0-11, R03-P2-2, R05-P0-5, R05-P1-17 | §6 |
| R09-A13, R09-G9, R02-Q14, R05-P1-14 | §7 |
| R09-A1–A5, R09-A17, R09-G3–G6, R09-G11, R04-P1-13–15, R05-P0-4, R05-P0-9, R03-P1-10 | §8 |
| R09-A9–A11, R09-A16, R09-G10, R02-Q12, R02-Q17, R03-P1-9, R01-Q14 | §9 |
| R09-A18 | §10 |
| R01-Q4, R01-Q19, R03 §8.1, R04-P2-23 | §11 |

**Dependencies on other sections.**
- **02-engine-runtime:** the `.hschema` grammar (`client{}`/`server{}`, `@store`), tag and HXL
  cook steps, `hmath` with strict FP, grids and `Reparent()`, Luau budgets, terrain modifications.
- **04-networking:** command frames, the owner channel with prediction keys, a ≥ 500 ms hitbox
  history, phase filtering, and cross-group messages and handoff payloads.
- **05-backend:** aligned with its §1.6–1.15, plus four additions: a Go HXL package, a world-state
  flags service, an Economy Sim service, and escrow refund timers for crafting sessions.
- **07-editor:** the graph compilers (T09, T12, T13), the AI and spawn tool, EV simulators, and the
  "view as state" debugger.
