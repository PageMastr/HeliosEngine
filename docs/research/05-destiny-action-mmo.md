# 05 — Destiny (Tiger Engine) & Action/Shooter MMO Technology

*Research report for the sci-fi MMO engine project. Topic owner: research agent 05. Date: 2026-09-25.*

> **How to read the evidence in this report.** Claims tagged **[V]** were checked this session against a retrieved source: a search-engine summary of the source or a fetched page. Claims tagged **[K]** come from well-known public talks and reporting that I could not re-fetch, because the network egress proxy blocked most sites (GDC Vault, gamedeveloper.com, bungie.net, PC Gamer, Kotaku and others). Treat [K] items as strong leads to confirm against the linked talk before they drive a decision. Untagged text is analysis or recommendation.

---

## 1. Overview

Destiny (2014) and Destiny 2 (2017–present) are the best-documented example of an **action-shooter feel delivered at MMO-like scale**. Bungie chose not to build a classic MMO. Instead it built a **"shared world shooter"**: moment-to-moment combat feels like a local console shooter, and a cloud layer quietly stitches strangers together, tracks mission progress and owns all persistent loot and progression. The central technical idea is to **split authority by the latency tolerance of each kind of data**:

| Data | Latency tolerance | Destiny's authority |
|---|---|---|
| Own movement, aiming, ability activation | ~0 ms (must feel local) | The owning client [V] |
| Physics and AI state of a zone ("bubble") | tens of ms | The **Physics Host**: a player console in D1, Bungie's data center in D2 [V] |
| Mission and public-event progression, zone script state | 100 ms+ | The cloud **Activity Hosts**. Consoles talk to them about 10 times per second [V] |
| Inventory, loot, currency, progression | seconds | Backend services, exposed publicly through the Bungie.net API [V] |

The other titles in scope took different points on the same spectrum. Warframe used peer-to-peer mission hosts. The Division and PlanetSide 2 each trusted the client a great deal, and each paid for it. New World built a server-authoritative spatial grid, but launched with severe scaling and exploit problems. Overwatch and Halo: Reach are the reference designs for prediction and hit registration. Anthem is the reference cautionary tale about engine and tool fit.

**Bottom line for us:** our tentative stack (C++20 zone servers with Jolt, plus Go services) can support this class of game. We should copy Destiny's **layered-authority, host-crash-tolerant activity model** and its **extract→prepare→submit job-graph renderer**. We should use the **Overwatch model (server-authoritative, predicted, lag-compensated)** instead of Destiny's client-authoritative movement, because a PC-first MMO with PvP and a real-money-adjacent economy cannot afford client authority. We should also treat **iteration-time tooling as a P0 engine feature**. Bungie's own history shows what happens otherwise.

---

## 2. Tiger Engine & Renderer

### 2.1 Origins and architecture lessons (Butcher, GDC 2015)
- Around 2008–2009 Bungie retired the Halo engine and built **Tiger**, meant to last as long as the Halo engine had (about 10 years) [V]. It shipped D1 on PS3, 360, PS4 and Xbox One, then D2 (PC port with Vicarious Visions), and is still in service after large upgrades [V].
- Chris Butcher's four lessons [V]:
  1. **Ship Early.** Tiger's first components shipped in 2010, four years before Destiny, likely inside Halo: Reach. That acted as a forcing function and was called "one of the best decisions made."
  2. **Code Beats Documents.** Large architectural change on a big team happens through working code and refactors, not design documents.
  3. **Bring Up In Order.** Bring systems online in dependency order.
  4. **Content Is Non-Linear.** Content volume and its interactions grow faster than the team expects.
- Other topics in the talk were data lifetime management, object system design and source-code layering [V]. *Implication:* enforce layering in our CMake targets (core ← platform ← render/physics ← game ← tools) from day one.

### 2.2 Job-graph multithreading (Genova, GDC 2015)
- Bungie turned "almost every part of Destiny's engine into a job graph, with only limited use of thread-based pre-emption" [V]. Its stated premise was that heterogeneous multicore consoles demand fine-grained task and data parallelism instead of one thread per system, and that "a game engine must be designed from the ground up for job-based multithreading" [V].

### 2.3 Multithreaded renderer (Tatarchuk, GDC 2015)
- An **Update/simulation** stage simulates frame N, runs visibility and prepares data. A **Render** stage runs in lockstep one frame behind and issues draws for frame N−1. Multiple copies of the render-relevant scene state are buffered [V].
- **Feature renderers** plug into the pipeline through a fixed interface. They extract dynamic data from game objects into **frame-packet render nodes**, then convert that data to GPU-friendly formats. Each phase entry point (per-frame, per-object, per-view extract, prepare, submit) becomes a job [V]. Visibility results size the frame packets. A "populate render nodes" job builds cache-coherent arrays that the rest of the job chain consumes [V].
- **Strict access rules:** feature renderers may read only render-node data and statically cached render-object data. That gives automatic double-buffering and safe parallelism without locks inside feature code [V]. The open-source Rust renderer *rafx* reimplements this design (render features, frame packet, submit packet, render phases, views with phase masks) and is a good public reference [V].
- Related Bungie talks worth reading ([V] for existence, content [K]):
  - Umbra 3 occlusion and visibility for level creation (GDC 2013)
  - the Destiny shader pipeline (GDC 2017)
  - the GPU particle architecture (SIGGRAPH 2017)
  - physically inspired shading in D2 (GDC 2018)
  - "Creating Content to Drive Destiny's Investment Game" (SIGGRAPH 2014). This one covers the gear-production pipeline that makes thousands of loot items affordable.

### 2.4 Engine evolution under live service
- The **Beyond Light (2020)** engine overhaul forced a full reinstall. Bungie paired it with the **Destiny Content Vault**, which removed old destinations and cut install size by 30–40% [V]. Bungie said the goals were faster patching and shorter internal build times: developer builds went from **"24+ hours to sub-12 hours"** [V].
- *Lesson:* even a mature AAA engine accumulated build and patch debt large enough that the studio removed paid content from the game to manage it.

---

## 3. Networking & Server Architecture

### 3.1 Destiny 1: "Shared World Shooter" (Truman, GDC 2015)
- **Goal:** "low-latency action gameplay, always-available drop-in cooperative missions, and seamless in-world interactions with strangers," built by merging Bungie's traditional **peer-to-peer** networking with a new **cloud server** architecture [V].
- **Spatial structure.** Each destination (planet) is split into **bubbles**, roughly zones joined by corridors or transitions. Players in a bubble form a **P2P mesh** that broadcasts positions directly [V]. Matchmaking connects bubbles so strangers can meet mid-mission without a lobby [V].
- **Hosts** [V]:
  - **Bubble Activity Host (cloud).** Owns zone-level state such as enemy counts and plate activation. It runs "all the ambient scripting," scripts the ambient AI encounters, controls respawn timing and placement of resource nodes and chests, and runs all public-event logic.
  - **Mission Activity Host (cloud).** Tracks progress for a mission, strike or raid.
  - **Physics Host (one per bubble).** Holds "most of the authoritative state for the Bubble," including physics and AI. In D1 it ran **on a player's console** to save server cost.
  - Consoles talk to Activity Hosts at about **10 Hz** [V].
- **Host migration.** When the Physics Host player leaves, the Bubble Activity Host **promotes another console**, and the new Physics Host must immediately **reconcile against the Activity Hosts** [V]. Because progress lives in the cloud, a host loss does not reset the mission. In public spaces, D1's average Physics Host migration interval was reportedly **about every 2 minutes 40 seconds** [V, via a gist citing the talk]. The talk also covered **host-handoff rules at regional boundaries** and ungraceful disconnects [V].

### 3.2 Destiny 2: "not dedicated servers"
- In D2 **both the Mission Host and the Physics Host moved to Bungie's data centers**, which ended visible host migration [V]. Bungie avoided the phrase "dedicated servers." In its words: *"Destiny 2 uses a hybrid of client-server and peer-to-peer technology… The server is authoritative over how the game progresses, and each player is authoritative over their own movement and abilities. This allows players the feeling of immediacy in all their moving and shooting."* [V, TWAB 2017-05-25 as reported]
- Consoles still simulate physics locally for responsiveness [V].
- **Costs of this model** [V/K]:
  - PvP "trading" (both players die) and lag-switch or teleport exploits, because position is player-authoritative [V].
  - Other players appear to warp under packet loss [V].
  - A long tail of P2P connectivity errors caused by NAT and routers. Bungie's animal-named error codes such as BEAVER are the familiar example [K].
  - On PC, cheating pressure eventually led Bungie to add kernel anti-cheat (BattlEye) [K].

### 3.3 Matchmaking & population
- Destiny separates three things [V/K]:
  - **Fireteam** (party) membership, which persists across activities.
  - **Activity** matchmaking: strikes are matchmade; raids originally were not, and D2 later added "Guided Games".
  - **Bubble population** matchmaking. This silently chooses which instance of a patrol zone or social space (the Tower) a fireteam lands in, weighing capacity, friends and connection quality.
- Transitions happen inside **designed traversal spaces** (corridors, orbit loads), so instance swaps are invisible.

### 3.4 Social spaces
- The Tower and other hubs are simply bubbles with no combat, more players and lighter replication [K].
- Warframe **Relays** follow the same idea: dedicated-server hubs, while missions stay peer-hosted [K].
- *Requirement:* hub instances need a separate replication profile: many avatars, cosmetics, emotes and low update rates.

---

## 4. Combat Feel & Netcode Techniques

### 4.1 What makes Destiny feel good
- **Zero-latency self.** Movement, jumps and ability casts are client-authoritative, so input never waits on the network [V]. Shooting AI is resolved at the shooter and reported [K]. That is the most extreme form of favor-the-shooter.
- **Aim assist.** It is a tunable per-weapon system, not a single toggle [V]:
  - *Bullet magnetism*: shots bend toward targets inside an invisible **aim-assist cone**. The weapon's Aim Assistance stat widens the cone angle; its Range stat extends the cone depth. It applies to all input devices.
  - *Reticle friction/slowdown*: controller-only stickiness, driven by weapon archetype and range.
  - In Lightfall Bungie added an **Airborne Effectiveness** stat that reduces accuracy and assist while airborne [V].
  - *Implication:* aim assist must be data-driven per weapon and per input device, and the **server must evaluate the same cone** when validating hits.
- **Abilities and cooldowns.** D2's 30th-Anniversary rework introduced **variable cooldown tiers**, base cooldown bands tuned per ability so one ability can change without breaking others [V]. Energy regenerates **passively** (stat-scaled) and **actively** (from kills, damage dealt and damage taken, through perks and mods) [V]. The "Subclass 3.0" rework moved abilities to a modular aspects/fragments socket model [V].
- **Enemy archetypes.** Halo-lineage "combat dance":
  - Each race has fodder, ranged, tank, elite and boss roles with readable health-bar tiers [K].
  - Champions (Barrier, Overload, Unstoppable) arrived in Shadowkeep. They are a **systemic counterplay layer** that designers attach to existing enemies [V].
  - Bungie's GDC 2022 talk *"1000 Hours of Difficulty"* (Alan Blaine) explains how D2 creates hundreds of hours of challenge by applying **systemic adjustments across seven difficulty categories** to existing activity and sandbox content, instead of authoring new encounters [V]. Examples are modifiers, power deltas, density and champions.

### 4.2 Reference netcode: Overwatch (GDC 2017)
Sources for this section: Tim Ford, "Gameplay Architecture and Netcode"; Dan Reed, "Networking Scripted Weapons and Abilities"; "Replay Technology in Overwatch". Talks [V]; details [K].
- **ECS** simulation, with the same movement and ability code running on client and server.
- Fixed **command frames**.
- The client runs **ahead** of the server by about RTT/2 plus a small buffer. It sends **all unacknowledged inputs redundantly** in every packet. The server keeps an **input buffer**. Under packet loss the server tells the client to **speed up its simulation slightly** (time dilation) to refill the buffer.
- **Misprediction** triggers a rollback to the last authoritative state and a replay of buffered inputs. ECS makes this tractable because predicted state is isolated in known components.
- **Favor-the-shooter hit registration.** The server **rewinds hitboxes** to what the shooter saw, which is why players sometimes die "behind cover." Shipping shooters generally **cap rewind** so very high-ping players must lead their shots.
- **Statescript.** Abilities are authored in a visual state-graph language. Its state is networked and **predicted and rolled back like movement**. This is the most important precedent for our "visual scripting graph": **ability graphs must be rollback-safe**.
- **Replays and killcams** reuse the recorded ECS state stream.

### 4.3 Reference netcode: Halo: Reach ("I Shot You First", Aldridge, GDC 2011)
- Talk [V]; details [K]. The model is host-authoritative with client prediction of the local player's actions.
- Replication splits into **state** (eventually consistent current values), **events** (transient) and **control/input** streams.
- A **per-object, per-client priority system** fits a tight upstream bandwidth budget.
- Specific **latency-hiding** treatments cover firing, grenades, melee and vehicles.
- It is the ancestor of Destiny's approach and the canonical treatment of bandwidth prioritization for a 16-player host.

### 4.4 Other modern references [V existence]
- Valorant's 128-tick servers and "Peeking into Netcode" (peeker's advantage math).
- Apex Legends' server deep dive.
- Tribes networking model (the ghost/event/move stream split).
- Unreal's networked physics fundamentals.

### 4.5 Synthesis: the combat netcode we need
1. Server-authoritative fixed-tick simulation for each zone/cell. Clients predict their own avatar, weapon and abilities with rollback and replay.
2. A redundant input stream, a server input buffer and adaptive client time dilation.
3. A hitbox history ring buffer, with lag-compensated raycasts for hitscan and capped rewind. Projectiles are spawned predicted and validated against a rewound hitbox.
4. For **PvE against AI**, we may *optionally* accept client-reported hits with server plausibility checks (Destiny-style feel). This must be a per-activity policy switch, never global.
5. A relevance and priority system (Halo) plus interest management (PlanetSide 2-scale battles, see §7).

---

## 5. Loot, Progression & Activity Systems — and Their Backend Needs

### 5.1 Systems
- **Rarity tiers:** Common, Uncommon, Rare, Legendary, Exotic. Exotics are unique, build-defining items with equip limits [K].
- **Power level:** gear-score progression. Power **deltas** between player and activity scale damage both ways, and endgame activities cap or lock the delta [K]. Weekly **powerful/pinnacle** sources give gated jumps and create the weekly retention loop [K].
- **Random rolls:**
  - Introduced in D1, removed at D2 launch, restored in Forsaken (2018) [V].
  - Implemented with the **socket/plug system**: an item definition declares sockets (barrel, magazine, perks, mods, shader, masterwork), and at drop time plugs are chosen from each socket's **legal plug set** [V]. Sockets can draw plugs from the item definition or from character-scoped and profile-scoped plug sets [V].
  - Plug insertion is rate-limited on the public API (initially 2 socket actions per second per user) [V].
- **RNG correctness incident.** In 2024–25 players proved that certain perk *combinations* dropped less often. Bungie traced it to a PRNG seeded with **sequential inputs**, whose patterns carried into outputs. The fix changed how rolls were seeded, using **salted hashing** [V].
- **Engrams:** drops are an unresolved "engram" that is decoded later. The actual roll is resolved server-side at a controlled point, which allows late tuning and focusing [K].
- **Activities** [K]:
  - **Strikes:** matchmade three-player dungeons with a boss. Nightfall adds weekly modifiers.
  - **Raids:** six players, not matchmade, with coordination mechanics, **checkpoints persisted server-side**, and **weekly loot lockouts**.
  - **Public events:** timed, run by the Bubble Activity Host; anyone in the bubble can join; optional objectives raise them to "Heroic."
  - **Social spaces:** vendors, bounties, postmaster.

### 5.2 Live-service lessons
- Truman's GDC 2022 talk on D2's shift from box product to live service compares a box product to building a **train** and a live service to running a **train station** that dispatches trains on a cadence [V].
- D2 lost trust at launch by "making it more accessible" and removing depth such as random rolls. Bungie then reworked "almost every aspect" of its live business [V].
- *Implication:* the backend must support **cadence**: seasons, weekly resets, rotating vendors and time-boxed events. These must be driven by data and schedules, not client patches.

### 5.3 Backend needs derived from Destiny
| Need | Destiny evidence | Our implementation |
|---|---|---|
| Static **definitions/manifest** with stable hash IDs, per-language files, versioned URL | Bungie API ships definitions as a large JSON file or a zipped SQLite database; `GetDestinyManifest` returns current per-language URLs; hash IDs are unique within a type [V] | Content-cook step emits a signed, versioned definitions bundle (SQLite plus JSON) consumed by client, zone servers, Go services and the public API |
| **Item instances** with sockets/plugs, instance IDs, per-character and account buckets | API item/instance model; consumables in account-level buckets [V] | PostgreSQL `item_instance` table (JSONB sockets) plus an append-only inventory ledger |
| **Authoritative loot resolution** | Engram decode; plug sets [V/K] | Zone/activity server requests a grant → Go inventory service executes an idempotent, transactional grant keyed by reward token |
| **Cadence / resets / milestones** | Milestones replaced "Advisors" in the API [V] | Live-ops calendar service (Go) that publishes reset events on NATS |
| **Public read API** with OAuth scopes and component queries | `GetProfile`/`GetCharacter`/`GetItem` with `?components=`; OAuth scopes; OpenAPI 2/3 spec [V] | Plan a component-based public API from day one; it pays off for community tools |
| **Activity state persistence** | Activity Hosts survive physics-host loss; raid checkpoints [V/K] | Activity state in Redis or NATS JetStream KV, snapshotted to Postgres at checkpoints |

---

## 6. Tools & Iteration

- **Grognok** was Bungie's collaborative world builder. It was shown at GDC 2013 building a Moon base, with **multiple artists and designers editing one world concurrently** and "real-time" lighting and terrain edits [V].
- Iteration was still famously bad. Kotaku's 2015 account: to move a resource node, a designer loaded the map **overnight (about 8 hours)**, opened it the next morning (**about 20 minutes**), made the change, then ran a **15–20 minute compile** "just to do a half-second change." Tools were described as "an inhibiting factor" [V].
- The takeaway is that **collaborative editing is not the same as fast iteration**. The bottlenecks were load, import and bake/compile, not the editor UI.
- Five years after launch, internal builds still took 24+ hours before the Beyond Light overhaul brought them under 12 hours [V].
- Butcher's "Content Is Non-Linear" is the root-cause framing: pipelines that work at vertical-slice scale fall over at shipping scale [V].
- **Snowdrop (Massive)** takes the opposite approach [K]. It is famous for being **node-graph-driven throughout**: materials, gameplay logic, AI, UI and procedural placement. Live editing is connected to the running game. This became a studio-wide advantage, and Ubisoft reused Snowdrop for multiple titles.
- **Overwatch Statescript** shows that designers can own abilities in a graph language *if the runtime makes it networking-safe* [K].

**Tooling targets we should set now (derived):**
- Script, data and tuning change reaches a running game in **2 seconds or less**.
- Placement edits in a running level show up **without a reload**.
- A single-zone incremental cook takes **under 60 seconds**.
- A full nightly cook of the world runs in **under 4 hours**, parallelized.
- A new engineer syncs, builds and gets into the game in **under 30 minutes**.
- Budgets are tracked in CI, and pipeline changes that break them fail the build.

---

## 7. Other Action MMOs

### 7.1 Warframe (Digital Extremes, Evolution engine) [K unless noted]
- **Networking.** Missions are **peer-hosted**, with one player as host and host migration. **Dedicated servers** run social hubs (Relays) and some PvP. The networking architecture is covered in a public talk by Maciej Sinilo [V existence].
- **Content.** **Procedural tilesets**: hand-authored rooms and "tiles" with typed connectors, assembled per mission. This gives cheap replayability and keeps authored quality.
- **Open landscapes.** Later open areas (Plains of Eidolon, Orb Vallis) required engine streaming work.
- **Lesson.** P2P hosting keeps a free-to-play game cheap to run, but host quality becomes everyone's problem. Hubs and PvP justify dedicated servers.

### 7.2 The Division 1/2 (Massive, Snowdrop) [K]
- **Engine.** Snowdrop is node-based throughout, with procedural city dressing, strong global illumination and weather.
- **Dark Zone.** A PvPvE sub-area with **extraction** (contaminated loot must be airlifted out), a **Rogue** state for players who attack others, and shared instances of a limited size. It is a compelling template for **opt-in PvP risk zones** in a PvE MMO.
- **Pitfall.** At PC launch in 2016, The Division trusted the client for many combat outcomes, reportedly to get responsiveness. Damage, rate-of-fire and teleport cheats spread quickly, and Massive spent much of Year 1 moving authority server-side.
- **Lesson.** Responsiveness should come from prediction, **not from trusting the client**.

### 7.3 PlanetSide 2 (SOE/Daybreak, ForgeLight) [K]
- **Scale.** Continents hold hundreds of players per side, and fights exceed 1,000 players; Guinness recognized a single battle of about 1,100+ players.
- **What makes it work:**
  - Aggressive **interest management**: distance and priority tiers, and a reduced update rate for far entities.
  - **Client-side hit detection** to hide latency at that scale.
  - Heavy engine work on CPU-bound rendering of large crowds.
- **Cost.** Client-side hit detection became a lasting cheating vector that needed server-side statistical detection.
- **Lesson.** At 1000+ players, spend replication budget by relevance, and plan anti-cheat around whatever authority you give away.

### 7.4 New World (Amazon Games; Lumberyard-derived "Azoth" engine) [K]
- **Architecture.** A server-authoritative MMO. Each world (about 2,000–2,500 concurrent players) is simulated across a **spatial grid of server processes** on AWS EC2, with cloud persistence (DynamoDB) and telemetry pipelines.
- **Launch problems (2021):**
  - Login queues in the tens of thousands, and frozen world transfers.
  - **Gold duplication** through trade and storage edge cases, which forced Amazon to disable wealth transfers.
  - An invulnerability exploit caused by stalling client updates.
  - HTML injection in chat.
  - A widely reported uncapped menu frame rate that stressed some high-end GPUs.
- **Lessons:**
  - Economy operations must be **transactional and idempotent across process boundaries**, especially at grid seams.
  - A client that stops sending data must never make the server treat that avatar as safe.
  - Load-test at 3–5× projected launch concurrency.
  - Sanitize all user text.
  - Cap frame rate in menus.

### 7.5 Anthem (BioWare, Frostbite) [K]
- **Engine fit.** Frostbite was built for Battlefield-style shooters and lacked RPG and loot infrastructure: inventory, saves and progression tooling. Kotaku's reporting describes months lost to building basics and a core loop (flight) that stabilized late.
- **At launch:** long load screens, loot-rate bugs and a thin endgame.
- **Lesson.** The engine must ship with **first-class RPG/loot/progression plumbing and tools**, not just rendering and shooting. That is exactly what this project is scoping.

---

## 8. Lessons & Pitfalls (consolidated)

1. **Split authority by latency tolerance.** Progression, loot and activity state go in durable cloud services. Physics and AI go in a simulation host. Own-avatar feel comes from prediction. A simulation-host crash must never cost progress, as Destiny's Activity Host design demonstrates [V].
2. **Client authority is a debt.** Destiny's PvP "trading" and lag-switching [V], The Division's cheats, PlanetSide 2's hit-detection cheats and New World's invulnerability exploit all trace back to trusting the client [K]. Prediction plus lag compensation gives nearly the same feel.
3. **P2P hosting has hidden costs:** migration roughly every 2m40s in D1 public spaces [V], NAT failures, and uneven host quality. D2 moved hosting to the cloud [V].
4. **Iteration time is a product feature.** An 8-hour load plus a 20-minute compile for a half-second change [V] slowed Destiny's content production for years.
5. **Build and patch debt compounds.** D2 cut 24+ hour builds to under 12 hours only after an engine overhaul and removing content [V].
6. **RNG is a correctness surface.** Seeding with sequential inputs biased perk combinations [V]. Test loot distributions statistically in CI.
7. **Economies need transactions.** New World's duplication bugs [K] show why the inventory and currency ledger must be the single source of truth.
8. **Systemic difficulty scales content.** Modifier and champion systems multiply hours per authored asset [V].
9. **Engine and genre fit matter.** Anthem [K].
10. **Launch capacity.** New World queues [K]. Horizontal world capacity, graceful queueing and transfer tooling must exist on day 1.
11. **Designed transition spaces hide instance swaps.** Destiny corridors and orbit loads are an example [V/K]. This is a level-design contract the engine and matchmaker must support.

---

## 9. Requirements for Our Engine (prioritized)

### Stack flags
- **LuaJIT for predicted gameplay is risky.** Ability and weapon logic must be **rollback-safe, deterministic per tick and snapshot-able**. That is hard with a GC'd, JIT-compiled VM whose state lives in an opaque heap.
  - *Recommendation:* author predicted abilities in the **visual graph compiled to a C++ state-machine VM** with explicit, serializable state (Statescript-style). Keep Lua for **non-predicted** server logic: quests, mission scripts and vendors.
  - Evaluate **Luau** as a better-maintained, sandboxed and typed alternative to LuaJIT.
- **Jolt:** fine. Use one `PhysicsSystem` per zone instance, and enable Jolt's cross-platform determinism build option for server/client consistency.
  - Lag compensation should use a **hitbox history buffer**, not a physics rewind.
- **Go + Postgres + Redis + NATS:** fine for this genre.
  - Put **loot roll resolution in the C++ activity/zone server** (it has the context), but put **grants in a Go inventory service** backed by a Postgres transactional ledger.
  - Use **NATS JetStream** (durable) for activity checkpoints and grant events, not core NATS alone.
- **No pure P2P.** Keep an optional relay only as a fallback transport.

### P0: required for a first playable vertical slice
1. **Job-graph core.** A fiber or job scheduler with dependency graphs. All engine systems run as jobs, and dedicated threads are limited to I/O and OS needs. (Genova [V])
2. **Pipelined renderer** on Vulkan 1.3:
   - simulate(N) → **extract** → **prepare** → **submit**(N−1);
   - pluggable **feature renderers**, frame packets and views with render-phase masks;
   - strict read-only access to render nodes (Tatarchuk [V]; the rafx design [V]);
   - GPU-driven visibility and occlusion (Destiny used Umbra [V]).
3. **Server-authoritative fixed-tick zone simulation** (30 or 60 Hz, configurable per zone type):
   - client prediction and reconciliation for avatar movement, weapons and abilities;
   - redundant input stream, server input buffer and adaptive client time dilation (Overwatch [K]).
4. **Lag-compensated hit registration:**
   - a per-entity hitbox history ring buffer of at least 500 ms;
   - rewound raycasts for hitscan, and predicted projectiles validated against rewound hitboxes;
   - a **configurable rewind cap**;
   - a per-activity policy flag for "PvE client-reported hits with plausibility checks".
5. **Layered authority and host-crash tolerance:**
   - **Activity/Mission service**: holds objective state, checkpoints and encounter phase in durable state (Redis/JetStream, then Postgres).
   - **Zone server**: holds physics and AI.
   - A replacement zone server can **rehydrate from activity state** within a few seconds (the D1 Physics Host plus Activity Host pattern [V]).
6. **Instance/bubble population service** (Go):
   - places fireteams into zone instances by capacity, social graph, latency and fireteam integrity;
   - supports **seamless transfers** at designer-marked transition volumes;
   - keeps fireteam membership persistent across activities.
7. **Data-driven definitions ("manifest"):**
   - the content cook emits versioned, hash-keyed definitions (SQLite and JSON, per language) shared by client, servers, services and the API [V].
   - Includes weapons, perks, socket/plug sets, loot tables, activities and modifiers.
8. **Item instance and socket/plug model** with a **transactional, idempotent grant ledger**:
   - grants are keyed by a reward token, so duplicates are impossible;
   - every mutation is audit-logged;
   - there is a rate limit on socket and plug operations [V].
9. **Weapon and ability sandbox, all data-driven:**
   - fire modes, recoil, spread and range falloff;
   - **per-weapon, per-input-device aim assist** (magnetism cone angle and depth, reticle friction and slowdown, airborne penalties) [V];
   - ability energy with passive and active regeneration and cooldown bands [V].
   - Server hit validation uses the same aim-assist cone.
10. **Hot-reload iteration loop:** tuning, data and Lua changes reach a running client and server in 2 seconds or less. Level placement edits in the running game need no reload. The iteration budgets in §6 are tracked in CI.
11. **Security baseline:** the server owns damage outcomes in PvP, and all currency, inventory and progression. Stalled clients get no protection. All user text is sanitized. Movement is validated (speed and teleport checks).

### P1: required before alpha and scale tests
12. **Rollback-safe visual ability graph** (Statescript-class), compiled to a C++ VM with serializable state and networked prediction [K].
13. **Relevance, priority and interest management:**
    - per-client, per-entity priority from distance, view and gameplay relevance;
    - bandwidth budgeting (Halo: Reach [K]);
    - tiered update rates for 200 to 1,000+ player battles (PlanetSide 2 [K]).
14. **Enemy AI archetype framework:**
    - roles (fodder, ranged, tank, elite, boss) and health-bar tiers;
    - **systemic modifiers**: champions-style counters, density, lethality and power-delta scaling, attachable to any activity (the "1000 Hours of Difficulty" approach [V]).
15. **Loot service:**
    - weighted tables with **statistically tested RNG** (salted/hashed seeding; CI distribution tests) [V];
    - pity and bad-luck protection counters, weekly lockouts, and powerful/pinnacle-style gated sources.
16. **Live-ops calendar:** seasons, daily and weekly resets, rotating vendors and timed public events, all published over NATS and driven by data.
17. **Public events and bubble scripting:** a server-side zone-scripting host for ambient encounters, resource-node respawns and public-event state machines (the Bubble Activity Host role [V]).
18. **Social hub replication profile:** high avatar count, low-rate cosmetic state and emote sync, split from combat zones.
19. **Replay and killcam recording** from the snapshot stream, also used for anti-cheat review [K].
20. **Load and chaos testing** of the population, activity and inventory services at 3–5× target concurrency, including zone-server kill tests.

### P2: differentiators and later
21. **Procedural tileset missions**: authored rooms with typed connectors (Warframe [K]).
22. **Opt-in PvP risk zones** with extraction and rogue rules (Dark Zone [K]).
23. **Public read-only web API**: component-based queries, OAuth scopes and an OpenAPI spec (the Bungie.net model [V]). This enables community tools and companion apps.
24. **Spatial grid handoff** for seamless large open worlds (New World-style), with transactional handoff of economy-relevant state across seams [K].
25. **Content vaulting and streaming-install support**, so install size and patch size can be managed over a 10-year service (DCV [V]).

---

## 10. Sources

### Retrieved or verified this session (search-engine summaries of the source, or fetched pages)
- Truman, *Shared World Shooter: Destiny's Networked Mission Architecture*, GDC 2015 — https://gdcvault.com/play/1022247/Shared-World-Shooter-Destiny-s ; video https://www.youtube.com/watch?v=Iryq1WA3bzw ; slides https://media.gdcvault.com/gdc2015/presentations/Truman_Justin_Shared_World_Shooter.pdf
- nessus42, "How Networking Works in Destiny 1 and How It Will Differ in Destiny 2 (According to Bungie)": https://gist.github.com/nessus42/f12f094e4abe30c0d00c9b4c86c387ce and https://gist.github.com/nessus42/df399f31e4ab41192cbd51b32e9d7b73 ; FAQ https://gist.github.com/nessus42/3738647a2052c4758c6f46f0464c47a6
- GamesRadar, "Here's how Destiny 2's servers will work" — https://www.gamesradar.com/heres-how-destiny-2s-servers-will-work-no-more-host-migration/
- PC Gamer, "The strange science of Destiny 2's netcode" — https://www.pcgamer.com/the-strange-science-of-destiny-2s-uniquely-complicated-netcode/
- Kotaku, "Bungie Explains How They're Improving Destiny 2 Servers" — https://kotaku.com/bungie-explains-how-theyre-improving-destiny-2-servers-1795587013
- Tatarchuk, *Destiny's Multithreaded Rendering Architecture*, GDC 2015 — https://www.gdcvault.com/play/1021926/Destiny-s-Multithreaded-Rendering ; slides https://advances.realtimerendering.com/destiny/gdc_2015/Tatarchuk_GDC_2015__Destiny_Renderer_web.pdf ; writeup https://www.gamedeveloper.com/programming/-i-destiny-i-s-multithreaded-rendering-architecture-explained-at-gdc-2015
- Genova, *Multithreading the Entire Destiny Engine*, GDC 2015 — https://www.gdcvault.com/play/1022164/Multithreading-the-Entire-Destiny ; video https://www.youtube.com/watch?v=v2Q_zHG3vqg
- Butcher, *Lessons from the Core Engine Architecture of Destiny*, GDC 2015 — https://gdcvault.com/play/1022106/Lessons-from-the-Core-Engine ; slides https://media.gdcvault.com/gdc2015/presentations/Butcher_DestinyEngine_GDC2015_final.pdf ; https://www.gamedeveloper.com/design/developers-of-i-destiny-i-i-ac-unity-i-share-lessons-learned-at-gdc-2015
- rafx renderer architecture (a Destiny-inspired open implementation) — https://github.com/aclysma/rafx/blob/master/docs/renderer/renderer_architecture.md
- Curated Destiny talk list (shader pipeline, particles, Umbra, investment-game content, PBR) — https://github.com/cohaereo/alkahest/blob/main/README.md
- Kotaku (Schreier), "The Messy, True Story Behind The Making Of Destiny" — https://kotaku.com/the-messy-true-story-behind-the-making-of-destiny-1737556731
- Grognok — https://bungie.fandom.com/wiki/Grognok ; https://rampancy.net/youtube-video/03302013/grognok-creator-destinys-worlds
- Beyond Light engine overhaul, DCV and install size — https://www.pcgamesn.com/destiny-2/beyond-light-install-size ; https://www.gfinityesports.com/article/destiny-2-beyond-light-pc-pre-load-steam-size-reinstall-engine-bungie ; https://www.thegamer.com/destiny2-beyond-light-smaller-download/
- Bungie.net API — https://github.com/Bungie-net/api ; manifest wiki https://github.com/Bungie-net/api/wiki/Obtaining-Destiny-Definitions-%22The-Manifest%22 ; item definition schema https://bungie-net.github.io/multi/schema_Destiny-Definitions-DestinyInventoryItemDefinition.html ; SocketPlugSources https://bungie-net.github.io/multi/schema_Destiny-SocketPlugSources.html
- Bungie, "Dev Insights: The Perk Weighting Issue" — https://www.bungie.net/7/en/News/article/dev_insights_perk_rng_issue ; https://www.windowscentral.com/gaming/destiny-2-players-may-have-just-found-a-wild-perk-weighting-bug
- Aim assistance — https://d2.destinygamewiki.com/wiki/Aim_Assistance ; https://www.thegamer.com/destiny-2-weapon-stats-explained/ ; Airborne Effectiveness https://www.thegamer.com/destiny-2-airborne-effectiveness-explained/
- Abilities and cooldowns — https://www.bungie.net/7/en/News/Article/ability-changes-lightfall-d2 ; https://www.thegamer.com/destiny-2-ability-super-cooldowns-explained/
- Blaine, *1000 Hours of Difficulty: How 'Destiny' Builds Systemic Challenge*, GDC 2022 — https://gdcvault.com/play/1027550/1000-Hours-of-Difficulty-How ; slides https://media.gdcvault.com/GDC+2022/Speaker+Slides/1000+Hours+of_Blaine_Alan.pdf
- Truman, *From Box Products to Live Service: How 'Destiny 2' Transformed Bungie*, GDC 2022 — https://gdcvault.com/play/1027599/From-Box-Products-to-Live ; https://www.youtube.com/watch?v=ZLbvMWEAoyY
- Networking resource indexes (source of the talk URLs below) — https://github.com/ThusSpokeNomad/GameNetworkingResources ; https://github.com/0xFA11/MultiplayerNetworkingResources

### Referenced talks: URLs verified via the indexes above, content summarized from prior knowledge [K]
- Aldridge, *I Shot You First: Networking the Gameplay of Halo: Reach*, GDC 2011 — https://www.gdcvault.com/play/1014345/I-Shot-You-First-Networking
- Ford, *Overwatch Gameplay Architecture and Netcode*, GDC 2017 — https://www.gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and
- Reed, *Networking Scripted Weapons and Abilities in Overwatch*, GDC 2017 — https://www.youtube.com/watch?v=ScyZjcjTlA4
- *Replay Technology in Overwatch*, GDC 2017 — https://www.youtube.com/watch?v=W4oZq4tn57w ; *Overwatch: Let's Talk Netcode* — https://www.youtube.com/watch?v=vTH2ZPgYujQ
- Sinilo, *Warframe Networking Architecture* — https://www.youtube.com/watch?v=VVetqMgcN50
- Riot, *Peeking into Valorant's Netcode* — https://technology.riotgames.com/news/peeking-valorants-netcode ; *Valorant's 128-Tick Servers* — https://technology.riotgames.com/news/valorants-128-tick-servers
- Respawn, *What Makes Apex Tick* — https://www.ea.com/en-au/games/apex-legends/news/servers-netcode-developer-deep-dive
- Frohnmayer and Gift, *The TRIBES Engine Networking Model* — https://www.gamedevs.org/uploads/tribes-networking-model.pdf
- Destiny rendering and content talks (from the alkahest list): Umbra 3, GDC 2013 https://gdcvault.com/play/1017834/Powering-up-Destiny-s-Level ; Shader Pipeline, GDC 2017 https://gdcvault.com/play/1024231/-Destiny-Shader ; Particle Architecture, SIGGRAPH 2017 https://advances.realtimerendering.com/s2017/Destiny_Particle_Architecture_Siggraph_Advances_2017.pptx ; Investment-game content, SIGGRAPH 2014 https://advances.realtimerendering.com/destiny/siggraph2014/bungie_gear_production_siggraph_2014_web_ready.pdf

### Not re-verified this session (egress blocked); confirm before relying on specifics [K]
- The Division/Snowdrop: Massive and Ubisoft GDC talks and 2016 reporting on client-trust cheating.
- PlanetSide 2/ForgeLight: SOE/Daybreak statements and the Guinness battle-size record.
- New World: AWS re:Invent 2021 architecture talk and 2021 launch reporting (queues, gold duplication, invulnerability, chat injection).
- Anthem: Kotaku (Schreier, 2019) reporting on Frostbite and development.
- Destiny P2P error codes (BEAVER and others) in Bungie Help, and Destiny 2 BattlEye adoption.
