# 01 — EVE Online and CCP's Technology (Carbon, Trinity, Destiny, Tranquility)

*Research report for the sci-fi MMO engine project. Status: complete. Date: 2026-09-25.*

> **How this was researched.** Most CCP pages (eveonline.com, developers.eveonline.com), fan wikis, and press sites were blocked by the sandbox egress proxy, so claims about those pages come from search-engine extracts of them (cited by their original URL). The newly open-sourced **Carbon engine repositories on GitHub** could be read directly, and they were the best primary source: repo layout, READMEs, and design docs. I studied architecture only. No source code was copied. A few points come from general domain knowledge and were not re-checked this session; they are marked **[BK]**.

---

## 1. Overview

EVE Online (CCP Games, Reykjavík; launched May 2003) is the reference design for a **single-shard, player-driven space MMO**. All players share one persistent universe, called **Tranquility (TQ)**, with thousands of solar systems. Nearly all economic and territorial outcomes come from players. The studio's 2026 milestones matter to us:

- **CCP became "Fenris Creations"** as an independent company in May 2026 ([GameFromScratch](https://gamefromscratch.com/carbon-game-engine-open-sourced/), [GamingOnLinux](https://www.gamingonlinux.com/2026/07/carbon-engine-framework-powering-eve-online-is-now-open-source/)).
- **The Carbon engine framework was open-sourced on 1 July 2026**, mostly under MIT. It spans 30+ modules, including Trinity (renderer), Destiny (space simulation and pathfinding), CarbonIO, the scheduler, Blue (the Python↔C++ glue), resources/patching, audio, and db ([github.com/carbonengine](https://github.com/carbonengine), [components list](https://github.com/carbonengine/documentation/blob/main/components.md), [Massively OP](https://massivelyop.com/2026/07/01/eve-onlines-fenris-creations-just-open-sourced-the-carbon-engine-framework-its-built-on/)). Economy and gameplay systems stay closed ([VGTimes](https://vgtimes.com/gaming-news/159966-eve-onlines-carbon-engine-goes-open-source-on-github-key-economy-systems-remain-closed.html)).
- **The Python 3 migration began in August 2026.** EVE has run on Stackless Python 2.7 since 2010. The codebase is about **2.4 million lines of Python in about 20,000 files**. Stage 1 uses python-future to make the code dual-compatible (95.9% of files already compiled under both interpreters). Stage 2 covers about 20,000 lines that parse fine but *behave* differently, such as integer division on damage, ISK, and coordinates, and needs human review ([dev blog](https://www.eveonline.com/news/view/the-move-to-python-3-begins), [Simon Willison](https://simonwillison.net/2026/Aug/25/eve-online-move-to-python-3/), [Talk Python #564](https://talkpython.fm/episodes/show/564/eve-online-departs-for-python-3)). EVE Frontier already runs on Python 3 via Carbon ([EVE Frontier blog](https://evefrontier.com/en/news/moving-into-the-future-upgrading-to-python-3)).

The core lesson for us is that EVE is a **C++ engine core with a very large dynamic-language game layer**. It scales by **spatial partitioning at the solar-system level** and a **very low simulation tick (1 Hz)**. When hardware runs out, it degrades gracefully by **slowing time (TiDi)** rather than dropping updates. The stack has lasted 23 years, and much of CCP's recent engineering has gone into paying down debt from early choices: a single-threaded interpreter per node, Python 2.7 lock-in, a monolith, and a Windows/SQL Server coupling.

---

## 2. Client / Engine Architecture

### 2.1 Carbon: the shared framework

Carbon began around 2009–2011 as CCP's shared technology for EVE, Dust 514, and the cancelled World of Darkness MMO. Traces survive in the open-sourced Trinity tree as `Interior/` and `Wod/` directories ([trinity/trinity](https://github.com/carbonengine/trinity/tree/main/trinity)). Today Carbon is "open-source technology for persistent sandbox worlds" behind EVE Online and EVE Frontier. Its module map ([components.md](https://github.com/carbonengine/documentation/blob/main/components.md), [org repos](https://github.com/orgs/carbonengine/repositories)):

| Layer | Carbon module | Role |
|---|---|---|
| Platform | `core`, `pdm`, `d3dinfo`, `ime`, `spacemouse`, `exefile` | OS abstraction, hardware detection, input, final executable (Windows `.cpp` + macOS `.mm`, Crashpad) |
| Object model / scripting glue | `blue`, `blueexposure` | Exposes C++ classes to Python, **persistence** (object graphs saved/loaded), resource loading of objects and dependencies ([blue](https://github.com/carbonengine/blue)) |
| Concurrency | `scheduler` | Channels + scheduler for **greenlet** coroutines, deliberately emulating **Stackless tasklet/channel semantics** so game code can move to stock CPython 3 ([scheduler](https://github.com/carbonengine/scheduler)) |
| Networking | `io`, `grpc` | Tasklet-blocking sockets/SSL (drop-in `socket` replacements), machoNet packet support ([io](https://github.com/carbonengine/io)); base for per-project Python gRPC modules ([grpc](https://github.com/carbonengine/grpc)) |
| Rendering | `trinity` (+ `trinityal`), `imageio`, `imagetools`, `videoplayer` | Renderer with a DX11/DX12/Metal abstraction layer |
| Simulation | `destiny`, `pathfinder` | Space physics ("balls" in a "ballpark"), collision, and route-finding over the star map |
| Math | `math`, `geo2`, `parser` | Vectors/quaternions exposed to Python (geo2 is built on DirectXMath), math-expression parser |
| Data | `db`, `fsd` (legacy) → cFSD, `localization` | Game-server DB access; static game data |
| Assets | `resources`, `mesh`, `red-to-black-converter` | Resource groups, patches, bundles; mesh/animation; text→binary object conversion |
| Audio | `audio`, `trinityaudioapi`, `spatial-audio-clustering` (Wwise plugin, Apache-2.0) | Audio engine |
| Ops | `prometheus` | "Native Prometheus client for the monolith" |

**Pattern:** every C++ module ships a `*_Blue.cpp` exposure file and a `python/<module>` package. In Destiny, for example, `Ballpark.cpp` pairs with `Ballpark_Blue.cpp` ([destiny/src](https://github.com/carbonengine/destiny/tree/main/src)). The engine is effectively a **library of C++ services driven by Python**: C++ owns the hot loops (rendering, physics, IO), and Python owns game rules, UI, and orchestration. Builds use **CMake + vcpkg** (a private vcpkg registry), CI runs on **TeamCity**, and some dependencies still come from **Perforce** (`CCP_EVE_PERFORCE_BRANCH_PATH`) ([destiny README](https://raw.githubusercontent.com/carbonengine/destiny/main/README.md)).

### 2.2 Stackless Python on client and server

- CCP's CEO said they could not, "given constraints of time and commercial reality, do this in a compiled language and we needed innovative concurrency control for such a large scale shared state simulation" ([Stackless wiki: Applications](https://github.com/stackless-dev/stackless/wiki/Applications)).
- Kristján Valur Jónsson's PyCon talk "Stackless Python in EVE" says most of the engine, except the renderer and physics, is Stackless Python. It relies on **tasklets** (cooperative, non-OS, non-preemptive microthreads) and **channels** (rendezvous for data passing and synchronization). Tasklet switches are far cheaper than thread switches, and huge numbers of tasklets can be live ([slides](https://www.slideshare.net/slideshow/stackless-python-in-eve/64850), [slideplayer copy](https://slideplayer.com/slide/8123277/)).
- The major cost is that **the Python part of each node is single-threaded** because of the GIL, so each server node effectively uses one core ([2008 dev blog via search](https://www.eveonline.com/news/view/my-node-was-equipped-with-the-following...)). Every later scaling project works around this.

### 2.3 Trinity renderer

- A C++ renderer with a `Tr2*` object family: render jobs, blitters, atlas textures, denoiser, debug renderer, and more. Its subsystems include `Curves`, `Controllers` (data-driven animation), `Particle`, `PostProcess`, `RenderJob` (a render pipeline scripted from Python), `Shader` plus a standalone `shadercompiler`, `Sprite2d`/`UI` (the Python UI draws through Trinity), `Font`, and **`Raytracing` and a `Denoiser`** ([trinity tree](https://github.com/carbonengine/trinity/tree/main/trinity)).
- The EVE-specific layer (`trinity/Eve`) covers `EveSpaceScene` and its render driver, `EvePlanet`, `EveStarfield`, `EveLensflare`, `EveDistanceField`, `EveOccluder`, `EveInstancedMeshManager`, `EveLODHelper`, turrets, `SpaceObjectFactory`, `VirtualCamera`, trigger volumes, and particle forces ([trinity/Eve](https://github.com/carbonengine/trinity/tree/main/trinity/Eve)).
- **Graphics API abstraction (`trinityal`)** has three backends: **`dx11/`, `dx12/`, `metal/`**, plus a `stub/` for headless use. **There is no Vulkan backend** ([trinityal](https://github.com/carbonengine/trinity/tree/main/trinityal)). CCP reached macOS by writing a **native Metal backend**, not a portability layer.
- Granny3D is an optional file-format dependency. Collada animation comes in through `dae-to-red` ([ccpgames org](https://github.com/ccpgames)).
- **Carbon UI** is a Python UI framework that "guides players cleanly through complex gameplay systems" ([GameFromScratch](https://gamefromscratch.com/carbon-game-engine-open-sourced/)). EVE's dense, spreadsheet-like UI (overview, market, industry) is a first-class engine concern, not a thin HUD.
- [BK] Timeline: Trinity 1 (2003) → Trinity 2 "premium graphics" (2007) → DX11 → 64-bit client → DX12 → native Metal macOS client (2020s). Upscalers (FSR/XeSS/DLSS) were added in the 2020s.

---

## 3. Server / Cluster Architecture

### 3.1 Process topology

The cluster has three layers ([2008 "My node was equipped…" blog](https://www.eveonline.com/news/view/my-node-was-equipped-with-the-following...); [TQ Tech III](https://www.eveonline.com/news/view/tranquility-tech-3)):

1. **Load balancers → Proxy nodes.** These are public-facing. They terminate client connections, handle sessions and routing, and fan out broadcasts.
2. **SOL nodes.** These run the simulation. In 2008 there were 90–100 SOL blades running **2 nodes each**, with each node a single CPU-bound EVE server process on one core. A node is "the lowest level of granularity."
3. **Microsoft SQL Server.** It is the backend "both for some of the business logic but primarily for storage" ([TQ Tech III](https://www.eveonline.com/news/view/tranquility-tech-3)), so there is significant stored-procedure logic.

All nodes run the same software (a "fleet of monoliths"). What differs is which **load-balancing units** each node is assigned ([Honeycomb case study](https://www.honeycomb.io/blog/ccp-games-modernize-migrate-codebase)).

### 3.2 How the universe maps to nodes

- **Solar systems are load-balancing units.** "Location nodes" host many systems, sometimes hundreds. The **mapping is recomputed daily at downtime** (11:00 UTC). If a node dies, its systems are remapped to live nodes ([Fixing Lag: Character Nodes](https://www.eveonline.com/news/view/fixing-lag-character-nodes); [long-downtime post-mortem](https://www.eveonline.com/news/view/behind-the-scenes-of-a-long-eve-online-downtime)).
- **Dedicated nodes:** Jita, and historically Motsu and Saila, got a whole blade. The second node on that blade was left idle so the busy system had the machine to itself.
- **Character nodes (2010)** are a separate node type. They hold operations and data tied to a character but not to a location, so lookups no longer burden location nodes. This raised Jita's capacity ([Fixing Lag: Character Nodes](https://www.eveonline.com/news/view/fixing-lag-character-nodes)). **Lesson: split by *entity affinity* (location vs. character vs. global service), not just by space.**
- **Reinforced nodes / Fleet Fight Notification.** Players confidentially tell CCP about planned big fights. The automated tool moves just the requested systems onto **reserve high-performance nodes at the next downtime** ([Fleet Fight Notification Tool](https://www.eveonline.com/news/view/fleet-fight-notification-tool), [changes](https://www.eveonline.com/news/view/changes-to-the-fleet-fight-notification-system)). This is **static, human-in-the-loop load balancing**: a system cannot move live mid-fight.
- **Brain in a Box (2015):** a character's "brain" (all skill effects computed through Dogma) used to be recomputed on every system entry. Now it is computed once and **transferred between nodes** on jump. This, together with a Dogma rewrite, made mass arrivals in a system much cheaper ([No Downtime – Again!](https://www.eveonline.com/news/view/no-downtime-again)).

### 3.3 Threading and I/O inside a node

- **Game logic** runs on one Python thread with thousands of tasklets. **DB calls are tasklet-blocking**: the Carbon `db` module has `TaskletBlockingIO`, `SessionPool`, and rowset/accessor classes, so a query parks only the calling tasklet while native threads do the I/O ([carbonengine/db](https://github.com/carbonengine/db)).
- **StacklessIO (2008)** moved network I/O into a tasklet-aware native layer. Jita went from being unresponsive at 800–900 pilots to playable at 800, with peaks near 1,400. At that point the Jita node ran out of memory, which pushed the move to **64-bit servers (EVE64)** ([StacklessIO blog](https://www.eveonline.com/news/view/stacklessio-or-how-we-reduced-lag), [EVE64](https://www.eveonline.com/article/eve64)).
- **CarbonIO + BlueNet (2011)**. CarbonIO does parallel read-ahead and moves **compression, encryption, packetization and send off the GIL** onto other cores. **BlueNet** is a C++ callback hook that lets non-Python modules such as Destiny send and receive data "without ever touching Machonet" (the Python messaging layer) ([CarbonIO & BlueNet](https://www.eveonline.com/news/view/carbonio-and-bluenet-next-level-network-technology-1)). **Lesson: the hot data path must bypass the scripting VM.**

### 3.4 Time Dilation (TiDi)

- It is introduced per node, in 2011–2012. When a node is overloaded, the **game clock for its solar systems slows**, to a floor of **10%**: 1 game second takes 10 real seconds ([Introducing TiDi](https://www.eveonline.com/news/view/introducing-time-dilation-tidi)).
- Rationale (CCP Veritas): "A large majority of the load in large engagements is tied to the clock — modules, physics, travel, warp-outs… spacing out time will lower their load impact proportionally." The controller watches processing delay and dilates hard during expensive spikes such as a fleet warp-in or drone launch, then eases back toward normal speed ([Engadget explainer](https://www.engadget.com/2011-04-22-eves-anti-lag-time-dilation-concept-explained.html), [DataCenterKnowledge](https://www.datacenterknowledge.com/servers/experiencing-heavy-server-load-just-slow-down-time)).
- Result: in the 2012 fights at F-9F6Q and 92D-OI, module response stayed **under 1 second** for most of the action. Before TiDi, delays of 20, 40, or 600 s had been seen ([TiDi – How's That Going?](https://www.eveonline.com/news/view/time-dilation-hows-that-going)).
- CCP's Erlendur Þorsteinsson called TiDi "a band-aid" ([TechRadar](https://www.techradar.com/news/computing/what-it-takes-to-run-the-biggest-space-mmo-1289646)). TiDi keeps fights **fair and deterministic** but does not add capacity.

### 3.5 Hardware and database scale

- 2013: about 3,936 GB RAM and 2,574 GHz of aggregate CPU ([TechRadar](https://www.techradar.com/news/computing/what-it-takes-to-run-the-biggest-space-mmo-1289646)).
- TQ Tech III (2016, London): **4× SQL Server machines**, each with 768 GB RAM, running a **2.5 TB database**. The upgrade moved to Windows 2012 R2 and SQL 2014 ([TQ Tech III](https://www.eveonline.com/news/view/tranquility-tech-3)).
- TQ Tech IV (2021–2023): DB servers are Dell R750s with 32 cores, **4 TB RAM**, and SQL Server 2019. The cluster sits behind **Cloudflare**, with Intel Xeon Gold 5122/5222 SOL machines ([TQ Tech IV](https://www.eveonline.com/news/view/tranquility-tech-iv), [DB upgrade](https://eveonline.com/article/full-suite-of-upgrades-to-the-tranquility-db-yippie)).
- [BK] Peak concurrent users were about 65,300 (2013). The Guinness-recognized battles (B-R5RB 2014; FWST-8/M2-XFE 2020–21) involved thousands of pilots in one system under 10% TiDi.

### 3.6 Quasar: moving off the monolith

- **Quasar (2021–22)** is a gRPC/**protobuf** message bus inside Carbon. It "combine[s] the message routing capabilities of the message bus with the lightning fast serialization of protocol buffers," and all transmission, serialization, and routing happen **outside the GIL** except one memory copy ([Introducing Quasar](https://www.eveonline.com/news/view/introducing-quasar), [Worthplaying](https://www.worthplaying.com/article/2021/10/6/news/128717-eve-onlines-implements-quasar-tech-to-accommodate-more-players-bigger-battles/)).
- New features run as **microservices on AWS**. In parallel, CCP is migrating from monolith to microservices and from on-prem to cloud, using **Honeycomb distributed tracing** to find where messages get stuck ([Honeycomb](https://www.honeycomb.io/blog/ccp-games-modernize-migrate-codebase), [LeadDev](https://leaddev.com/technical-direction/how-eve-online-uses-observability-ease-migrations)).
- The public **ESI** REST API reaches the monolith over **RabbitMQ** through two channels: Quasar protobuf messages and legacy JSON ([The ship of ThESIus](https://developers.eveonline.com/blog/the-ship-of-thesius)).
- Gameplay code **emits protobuf events to Quasar**. Data engineering lands them in a "cold data lake" (up to about 1 hour latency), and designers review Tableau dashboards built from it ([Nosy Gamer on Carbon/Quasar](https://nosygamer.blogspot.com/2025/09/eves-carbon-engine-quasar-and-data.html)). **Telemetry is part of the engine, not a bolt-on.**

---

## 4. Networking Model

- **Transport:** TCP from client to proxy using the machoNet packet protocol, with compression and encryption handled off-GIL (CarbonIO). Proxies route calls to the owning SOL node by service or location. **Jumping systems means a session change to a different node**, which is why brain transfer mattered.
- **Server-authoritative space sim at 1 Hz.** Destiny advances physics once per second. The rate was chosen to keep bandwidth low enough for huge fights, and it drives module activation, locking, and status updates ([Power of Pretty](https://www.eveonline.com/news/view/paint-your-ship-red-and-make-it-faster)).
- **Deterministic shared simulation.** Destiny "runs both on the server and on each client," intended to produce identical results. A client joining a scene starts from the same variable values as the server, and afterwards the server mostly sends **commands and events** (orbit, approach, warp, stop) rather than per-frame transforms. Both sides integrate. Desync, caused by divergent inputs, ordering, or floating-point differences, was a long-running bug class that CCP had to fix explicitly ([Facing Destiny](https://www.eveonline.com/news/view/facing-destiny)). Point-and-click, momentum-heavy "ball" movement hides the 1 Hz tick well. This **would not** work for twitch FPS combat.
- **Interest management = grids.** A solar system is split into dynamic cubic **grids**, usually about 8,000 km across; the Jita 4-4 hub grid is about 20,000 km. They are created, merged, and resized as objects move ([Grid Sizes & You](https://www.eveonline.com/news/view/grid-sizes-you)). Updates are **fanned out once per simulation frame to everyone in the same grid ("bubble")**. Fanout is O(N×M) in a big fight, so even cosmetic changes are throttled: SKIN changes propagate at most once per second ([Power of Pretty](https://www.eveonline.com/news/view/paint-your-ship-red-and-make-it-faster)).
- **Heavy projectiles as simulated objects.** Missiles are Destiny balls, so large missile fleets multiplied physics cost. See "Fixing Lag: Drakes of Destiny" ([link](https://www.eveonline.com/article/fixing-lag-drakes-of-destiny-part-1-1)) and "Module Lag – Why Not All Bugfixes Are A Good Idea" ([link](https://www.eveonline.com/news/view/fixing-lag-module-lag-why-not-all-bugfixes-are-a-good-idea)).
- **External API (ESI):** public REST with OAuth SSO scopes. Rate limiting uses a floating window per (route-group, app:character or IP). Token cost depends on status: 2xx=2, 3xx=1, 4xx=5, 5xx=0. Responses carry `X-Ratelimit-*` headers and Retry-After, plus a legacy 100-errors/minute limiter returning 420. Clients must honor `Expires` and `ETag`/`If-None-Match` ([rate-limiting.md](https://raw.githubusercontent.com/esi/esi-docs/main/docs/services/esi/rate-limiting.md), [best-practices.md](https://raw.githubusercontent.com/esi/esi-docs/main/docs/services/esi/best-practices.md)). A large third-party tool ecosystem (killboards, market tools, fitting tools) depends on this API. It is effectively a product surface.

---

## 5. Gameplay & Economy Systems (what the engine/backend must support)

- **Dogma (attributes, effects, modifiers).** Every item type has numeric attributes. Effects, from modules, skills, ship bonuses, and fleet boosts, apply modifiers using ordered operators (`pre_assign, pre_mul, pre_div, mod_add, mod_sub, post_mul, post_div, post_percent, post_assign`) with **stacking penalties**, and the results depend on item state (offline/online/active/overload) ([dogma-engine reimplementation](https://github.com/EVEShipFit/dogma-engine), [ESI Dogma docs](https://docs.esi.evetech.net/docs/dogma.html)). **Dogma is the single most important gameplay data system to copy.** It is data-driven, fully server-side, and the same engine is used client-side for fitting previews.
- **Static data** (types, groups, categories, blueprints, map) is exported for third parties as the SDE. Internally it was authored as **FSD**, now replaced by **cFSD** ([carbonengine/fsd](https://github.com/carbonengine/fsd), [Fuzzwork on the SDE](https://www.fuzzwork.co.uk/2021/07/17/understanding-the-eve-online-sde-1/)).
- **Market:** [BK] Order books are per region, with buy and sell limit orders, order ranges (station/system/jumps/region), broker fees, and sales tax. There are no NPC sell orders for most goods, and the market concentrates in hubs (Jita 4-4, whose load justified a dedicated node and a larger grid). **Backend implication:** a transactional, auditable order-matching service; regional partitions; very high write rates at hubs; data export for third parties.
- **Economy management:** [BK] CCP hired an in-house economist (Dr. Eyjólfur Guðmundsson, 2007) and publishes **Monthly Economic Reports**, which track faucets (NPC bounties, mission rewards, ESS, incursions), sinks (taxes, broker fees, NPC-seeded skillbooks/blueprints, structure fuel), mining and production, and destruction. Destroyed ships are the main *material* sink. **Implication:** every ISK and item movement must be logged with a reason code from day one. Quasar events into a data lake are how CCP does this now.
- **Industry:** [BK] BPO/BPC blueprints, ME/TE research, invention, reactions, planetary production, and jobs that run for hours or days. **Implication:** durable timers, job queues, and scheduled completion events that survive restarts and node remaps.
- **Corporations, alliances, sovereignty, war:** [BK] player organizations with roles, wallets, hangars, and taxes; war declarations; **structures with reinforcement timers** that schedule fights days ahead (Upwell structures, sovereignty hubs); territorial control (entosis-based "Fozziesov" in 2015; the 2024 Equinox resource-based sov); killmails. Because reinforcement timers are known in advance, CCP can **pre-provision nodes** (Fleet Fight Notification). **Game design and load planning are coupled.**
- **Navigation:** stargate graph pathfinding (Dijkstra variants and flood-fill with routing preferences) in C++ ([carbonengine/pathfinder](https://github.com/carbonengine/pathfinder)).

---

## 6. Graphics & Art Direction

- **Scale and readability over realism.** Ships are big silhouettes against luminous, colored **nebula skyboxes**. Space is almost never black, and each region has a distinct palette. The rendering supports that with starfields, planets, lensflares, distance fields, and occluders as first-class `Eve*` scene types ([trinity/Eve](https://github.com/carbonengine/trinity/tree/main/trinity/Eve)).
- **Ship assembly through the Space Object Factory (SOF).** A ship is built from **hull + faction + race** data, not one baked asset. This is how CCP ships faction variants and cosmetic **SKINs** cheaply, and it shows up in CCP's WebGL port demos ("Ship loading via Space Object Factory," T3 composite ships, turret fitting/firing) ([ccpwgl](https://github.com/ccpgames/ccpwgl)). [BK] The "V3" ship texture overhaul (around 2010–2012) and later PBR shader updates reworked materials fleet-wide, which a data-driven SOF makes affordable.
- **Instancing and LOD for thousand-ship scenes:** `EveInstancedMeshManager` and `EveLODHelper`, plus client-side limits on effects and brackets in huge fights. The UI also draws through Trinity (`Sprite2d`, bracket sprites).
- **Modern features:** a DX12 backend, Metal, a ray-tracing module, and a denoiser in Trinity ([trinity tree](https://github.com/carbonengine/trinity/tree/main/trinity), [trinityal](https://github.com/carbonengine/trinity/tree/main/trinityal)).

---

## 7. Tools / Editors & Content Pipeline

- **Object persistence through Blue.** Scene and asset object graphs (Trinity objects, effects, curves) are saved as text **`.red`** files and converted to binary **`.black`** files for shipping ([red-to-black-converter](https://github.com/carbonengine/red-to-black-converter), [Blue](https://github.com/carbonengine/blue)). **Text for diffing and merging, binary for runtime.** Our engine should do the same.
- **Jessica.** [BK] CCP's in-house, Python-driven authoring tool, built on the same Blue and Trinity runtime. It is used to inspect and edit `.red` object graphs, effects, curves, and scenes. Because the tool embeds the real engine, **what you edit is exactly what ships**. There is no separate editor-only renderer.
- **Static data authoring:** FSD, then cFSD, with a C++ runtime reader and Python extension ([fsd](https://github.com/carbonengine/fsd)).
- **Assets:** `mesh` (mesh, animation, storage), `imagetools` (texture compression), Granny animation, Collada import (`dae-to-red`), and a Wwise audio pipeline ([org](https://github.com/orgs/carbonengine/repositories)).
- **Build and CI:** CMake, vcpkg with a private registry, TeamCity, Perforce for game content, clang-format/clang-tidy, and **Linux containers for building Carbon** (`linux-containers`) even though the client targets Windows and macOS ([org](https://github.com/orgs/carbonengine/repositories), [trinity](https://github.com/carbonengine/trinity)).
- **Testing:** [BK] a public test server (Singularity) and scheduled **mass tests with players** before big features. Destiny ships a `tools/benchmark` directory ([destiny](https://github.com/carbonengine/destiny)).
- **Observability:** a native Prometheus client inside the monolith ([prometheus](https://github.com/carbonengine/prometheus)) plus Honeycomb traces across the monolith and microservices.

---

## 8. Patching / Launcher

The **`resources`** library documents the whole scheme ([carbonengine/resources](https://github.com/carbonengine/resources)):

- **Resource Groups** are YAML manifests. They evolved from the older `resfileindex.txt` used by EVE and Frontier. Each entry has *RelativePath, MD5 Checksum, Uncompressed and Compressed size, CDN Location, Type, and BinaryOperation* ([resourceGroupFileFormat.rst](https://raw.githubusercontent.com/carbonengine/resources/main/doc/source/DesignDocuments/resourceGroupFileFormat.rst)).
- **Content-addressed CDN layout:** `[2-char prefix]/[FNV-hash-of-path]_[MD5-of-data]`. A new file version gets a new URL, so there is no cache invalidation, only dedup and cache-busting ([filesystemDesign.rst](https://raw.githubusercontent.com/carbonengine/resources/main/doc/source/DesignDocuments/filesystemDesign.rst)).
- **PatchResourceGroups** list **binary diffs** from a base group to the next version. **BundleResourceGroups** chunk data into evenly sized pieces for transfer, split by compressed size by default or by uncompressed size for speed ([bundles.rst](https://raw.githubusercontent.com/carbonengine/resources/main/doc/source/DesignDocuments/bundles.rst), [HowToCreateAPatch](https://raw.githubusercontent.com/carbonengine/resources/main/doc/source/Guides/HowToCreateAPatch.rst)).
- **The launcher embeds the resources library**, compares the local build against the CDN, and applies patches ([patchingProcess.rst](https://raw.githubusercontent.com/carbonengine/resources/main/doc/source/OverviewDocuments/patchingProcess.rst)). [BK] The EVE launcher keeps a **shared cache** of ResFiles across installs and accounts and can fetch missing resources on demand. Code and server deploys are tied to the **daily downtime**.
- Include/exclude filter rules pick platform-specific content ([fileFiltering.rst](https://raw.githubusercontent.com/carbonengine/resources/main/doc/source/DesignDocuments/fileFiltering.rst)).

---

## 9. EVE Frontier, Dust 514, and Talks

- **EVE Frontier** is a survival space sandbox built on Carbon and Python 3. Its world state is exposed to player code through **"World Contracts"** that represent characters, assemblies, items, and killmails on-chain. Players build **Smart Assemblies** (turrets, gates, storage, markets) with third-party dApps. The contracts moved from Solidity/MUD (archived `world-chain-contracts`) to **Sui Move**, plus zk location-proof experiments ([evefrontier org](https://github.com/evefrontier), [builder docs](https://github.com/evefrontier/builder-documentation), [projectawakening](https://github.com/projectawakening)). **Transferable lesson (setting blockchain aside):** a *sandboxed, permissioned player-programmable layer* on top of authoritative server state.
- **Dust 514** [BK] was a PS3 free-to-play FPS (2013–2016) on Unreal Engine 3, wired into the same Tranquility universe. EVE ships could bombard ground battles from orbit, and planetary conquest fed sovereignty. The lessons usually drawn: **cross-game integration was shallow and one-directional**, it had to cope with different platforms, audiences, and patch cadences, and a console client could not iterate as fast as the PC MMO. For us, an FPS layer (the Destiny/Star Citizen ambition) must be **native to the same engine, simulation, and backend**, not a separate game bolted onto shared persistence.
- **Talks to mine** (for the team to watch): Kristján Valur Jónsson's PyCon "Stackless Python in EVE" ([slides](https://www.slideshare.net/slideshow/stackless-python-in-eve/64850)); Honeycomb's "Modernizing a 20-year-old codebase with observability" (Nick Herring) ([link](https://www.honeycomb.io/resources/modernizing-a-20-year-old-codebase-with-observability-thanks)); Talk Python #564 on the Python 3 move ([link](https://talkpython.fm/episodes/show/564/eve-online-departs-for-python-3)); and Fanfest/EVE Vegas "Tranquility Tech" sessions, which match the TQ Tech III/IV blogs.

---

## 10. Lessons & Pitfalls (what CCP had to undo)

1. **A single-threaded interpreter per node is the root bottleneck.** StacklessIO, CarbonIO/BlueNet, character nodes, Brain in a Box, and Quasar all move work *off the GIL* or *off the node*. TiDi is the admitted "band-aid."
2. **Language-version lock-in.** 16 years on Python 2.7 left about 2.4M lines to migrate, where the dangerous part is **silent semantic changes** in damage, ISK, and coordinate math. Stackless itself became a dependency CCP had to replace (greenlet + carbon-scheduler).
3. **Static, downtime-bound load balancing.** A solar system cannot move between nodes live. Big fights have to be predicted, whether by players filling in a form or by timer mechanics.
4. **One tick rate for everything.** 1 Hz suits spaceship "ball" combat and makes deterministic lockstep cheap. It rules out twitch gameplay, which Dust needed a separate engine for.
5. **Deterministic client/server sim needs strict discipline**: the same code, inputs, ordering, and floating-point behavior. Otherwise you get desync ("Facing Destiny").
6. **Fanout is the scaling wall in big fights.** Even cosmetic changes had to be rate-limited, and missiles modeled as full physics objects blew up server cost.
7. **The monolith and the DB carry business logic.** Stored procedures in SQL Server plus a single codebase slowed change. Quasar and microservices are the corrective step.
8. **Sunk cost in abandoned verticals.** `Interior/` and `Wod/` rendering code (Incarna, World of Darkness) and Dust 514 show how side ventures consume core-tech bandwidth.
9. **Observability was retrofitted.** Honeycomb tracing was described as going from "a neighborhood" to "a house and a room." Build it in.

---

## 11. Requirements for Our Engine

### P0 — architecture decisions to make now

1. **Spatial load-balancing unit = "zone/cell"** (a solar system or sub-region). Zones must be **live-migratable** between server processes by snapshot and handoff, with no downtime. This fixes EVE's biggest ops limitation.
2. **Separate load-balancing axes:** location (zone servers), character/"brain" (a character service that owns computed stats and moves with the player), and global services (market, chat, guilds as Go services). Transfer computed character state on zone change instead of recomputing it (Brain in a Box).
3. **Per-zone configurable tick rate:** 1–2 Hz for strategic space and capital fleets, 20–60 Hz for FPS and dogfighting instances. All timed gameplay (cooldowns, cycles, travel) runs off a **per-zone dilatable game clock**, which gives us **TiDi as a built-in feature** with a 10% floor, visible to clients, driven by tick-overrun telemetry.
4. **The hot path never goes through the script VM.** Physics (Jolt), replication, serialization, compression, encryption, and fanout run in C++ on worker threads. LuaJIT only handles rules and events (the BlueNet lesson). Script tasks are coroutines (LuaJIT coroutines map directly to tasklets/channels) with **DB and service calls that block only the coroutine**.
5. **Multi-core per zone from day one:** a job system, with Jolt, interest management, and serialization parallelized. **Never assume "one process = one core."**
6. **Interest management by dynamic spatial grids/bubbles** with per-client priority and rate limits. Budget fanout explicitly (for example, cosmetic updates at ≤1 Hz and aggregated combat events).
7. **A Dogma-style attribute/modifier engine** in C++: data-driven types and attributes, ordered operator stages, stacking penalties, state-dependent effects. The same library runs on server and client for fitting previews.
8. **Every currency and item transfer is logged** with a reason code into an event stream (NATS → data lake) to support economic reporting (faucets and sinks).
9. **Content-addressed patching** (resource-group manifests, hash-named CDN objects, binary diffs, chunked bundles, shared cache, on-demand streaming) designed into the asset system early.

### P1 — needed for launch-quality MMO

10. **Deterministic, command-based replication for "space-scale" movement** (orbit, approach, warp), with periodic authoritative state correction and desync telemetry. Use snapshot interpolation plus client prediction for FPS-scale.
11. **Schema-first service messaging:** protobuf/gRPC between Go services and zone servers over NATS, with tracing (OpenTelemetry) through the C++ servers from the start.
12. **Hot state in memory, PostgreSQL as system of record** with write-behind, and **no gameplay logic in stored procedures**. Partition market and ledger tables by region.
13. **Durable timers and job scheduling** (industry, structure reinforcement, sov events) as a backend service that survives zone migration and restarts.
14. **Pre-provisioning hooks:** gameplay timers such as "siege at T+48h" feed the orchestrator so it scales zones *before* fights, automatically rather than through a form.
15. **Data-driven ship assembly (an SOF equivalent):** hull + faction + material/SKIN layers composed at load, with instancing and LOD for thousands of ships.
16. **Text-diffable asset/object format with binary cooking** (the `.red`→`.black` pattern). The editor runs the real renderer and simulation.
17. **A public, rate-limited, cacheable read API** (ESI-style: ETag/Expires, per-app buckets, OAuth scopes) for third-party tools.
18. **An RHI abstraction:** Vulkan 1.3 primary, with a stub/headless backend for servers and CI. Plan a **native Metal backend** only if macOS matters (CCP chose native Metal over translation).

### P2 — later or differentiators

19. A sandboxed player-programmable layer for structures and automation (the EVE Frontier idea, without blockchain), with strict permissions and metering.
20. Ray-traced effects and denoiser, upscalers.
21. Players or admins able to request **zone reinforcement** (a Fleet-Fight-Notification equivalent) as an ops tool on top of automatic scaling.
22. A mass-test harness: headless bot clients driving thousands of entities, plus benchmark tools per module (like Destiny's `tools/benchmark`).

### Stack flags from this research

- **LuaJIT: keep it, but contain it.** CCP's pain came from one VM thread per node and a frozen language version. Plan for **one Lua state per zone/worker thread**, keep the engine-script boundary narrow and bound by codegen, and consider Luau as a hedge because LuaJIT upstream is slow-moving and stuck on Lua 5.1 semantics. This mirrors the Python 2.7 trap.
- **Vulkan 1.3: fine for Windows and Linux.** Note that CCP's production renderer has **no** Vulkan backend (DX11/DX12/Metal). macOS would need MoltenVK or a native Metal backend.
- **Go + PostgreSQL + NATS: consistent with where CCP is heading** (Quasar microservices, gRPC/protobuf, message bus, cloud). Keep business logic out of SQL, and standardize on protobuf schemas over NATS.

---

## 12. Sources

**Primary, open source (read directly):**
- Carbon engine org: https://github.com/carbonengine ; repos list: https://github.com/orgs/carbonengine/repositories
- Components doc: https://github.com/carbonengine/documentation/blob/main/components.md
- Destiny: https://github.com/carbonengine/destiny ; src: https://github.com/carbonengine/destiny/tree/main/src ; README: https://raw.githubusercontent.com/carbonengine/destiny/main/README.md
- Trinity: https://github.com/carbonengine/trinity ; https://github.com/carbonengine/trinity/tree/main/trinity ; https://github.com/carbonengine/trinity/tree/main/trinityal ; https://github.com/carbonengine/trinity/tree/main/trinity/Eve
- Scheduler: https://github.com/carbonengine/scheduler ; IO: https://github.com/carbonengine/io ; Blue: https://github.com/carbonengine/blue ; gRPC: https://github.com/carbonengine/grpc ; DB: https://github.com/carbonengine/db ; FSD: https://github.com/carbonengine/fsd ; Pathfinder: https://github.com/carbonengine/pathfinder ; exefile: https://github.com/carbonengine/exefile ; Prometheus: https://github.com/carbonengine/prometheus ; red→black: https://github.com/carbonengine/red-to-black-converter
- Resources/patching: https://github.com/carbonengine/resources ; https://raw.githubusercontent.com/carbonengine/resources/main/doc/source/DesignDocuments/resourceGroupFileFormat.rst ; https://raw.githubusercontent.com/carbonengine/resources/main/doc/source/DesignDocuments/filesystemDesign.rst ; https://raw.githubusercontent.com/carbonengine/resources/main/doc/source/DesignDocuments/bundles.rst ; https://raw.githubusercontent.com/carbonengine/resources/main/doc/source/DesignDocuments/fileFiltering.rst ; https://raw.githubusercontent.com/carbonengine/resources/main/doc/source/OverviewDocuments/patchingProcess.rst ; https://raw.githubusercontent.com/carbonengine/resources/main/doc/source/Guides/HowToCreateAPatch.rst
- CCP GitHub (ccpwgl, dae-to-red, carbon-io/scheduler/core linux): https://github.com/ccpgames ; https://github.com/ccpgames/ccpwgl
- ESI docs: https://github.com/esi/esi-docs ; https://raw.githubusercontent.com/esi/esi-docs/main/docs/services/esi/rate-limiting.md ; https://raw.githubusercontent.com/esi/esi-docs/main/docs/services/esi/best-practices.md
- EVE Frontier: https://github.com/evefrontier ; https://github.com/evefrontier/builder-documentation ; https://github.com/evefrontier/world-contracts ; https://github.com/projectawakening
- Stackless applications wiki: https://github.com/stackless-dev/stackless/wiki/Applications
- Dogma reimplementation: https://github.com/EVEShipFit/dogma-engine

**CCP dev blogs (cited via search extracts; direct fetch blocked):**
- My node was equipped with the following… (2008): https://www.eveonline.com/news/view/my-node-was-equipped-with-the-following...
- StacklessIO or: How We Reduced Lag: https://www.eveonline.com/news/view/stacklessio-or-how-we-reduced-lag ; EVE64: https://www.eveonline.com/article/eve64
- Fixing Lag: Character Nodes: https://www.eveonline.com/news/view/fixing-lag-character-nodes ; Drakes of Destiny: https://www.eveonline.com/article/fixing-lag-drakes-of-destiny-part-1-1 ; Module Lag: https://www.eveonline.com/news/view/fixing-lag-module-lag-why-not-all-bugfixes-are-a-good-idea
- CarbonIO and BlueNet: https://www.eveonline.com/news/view/carbonio-and-bluenet-next-level-network-technology-1
- Introducing TiDi: https://www.eveonline.com/news/view/introducing-time-dilation-tidi ; TiDi – How's That Going?: https://www.eveonline.com/news/view/time-dilation-hows-that-going
- Facing Destiny: https://www.eveonline.com/news/view/facing-destiny ; Grid Sizes & You: https://www.eveonline.com/news/view/grid-sizes-you ; Server Deep Dive – The Power of Pretty: https://www.eveonline.com/news/view/paint-your-ship-red-and-make-it-faster
- No Downtime – Again! (Brain in a Box): https://www.eveonline.com/news/view/no-downtime-again
- Fleet Fight Notification Tool: https://www.eveonline.com/news/view/fleet-fight-notification-tool ; changes: https://www.eveonline.com/news/view/changes-to-the-fleet-fight-notification-system
- Behind the Scenes of a long downtime: https://www.eveonline.com/news/view/behind-the-scenes-of-a-long-eve-online-downtime
- Tranquility Tech III: https://www.eveonline.com/news/view/tranquility-tech-3 ; Tranquility Tech IV: https://www.eveonline.com/news/view/tranquility-tech-iv ; DB upgrade: https://eveonline.com/article/full-suite-of-upgrades-to-the-tranquility-db-yippie
- Introducing Quasar: https://www.eveonline.com/news/view/introducing-quasar ; The ship of ThESIus: https://developers.eveonline.com/blog/the-ship-of-thesius
- The Move to Python 3 Begins!: https://www.eveonline.com/news/view/the-move-to-python-3-begins ; EVE Frontier Python 3: https://evefrontier.com/en/news/moving-into-the-future-upgrading-to-python-3

**Secondary coverage:**
- TechRadar, "What it takes to run the biggest space MMO": https://www.techradar.com/news/computing/what-it-takes-to-run-the-biggest-space-mmo-1289646
- Engadget TiDi explainer: https://www.engadget.com/2011-04-22-eves-anti-lag-time-dilation-concept-explained.html ; DataCenterKnowledge: https://www.datacenterknowledge.com/servers/experiencing-heavy-server-load-just-slow-down-time
- Honeycomb case study: https://www.honeycomb.io/blog/ccp-games-modernize-migrate-codebase ; talk: https://www.honeycomb.io/resources/modernizing-a-20-year-old-codebase-with-observability-thanks ; LeadDev: https://leaddev.com/technical-direction/how-eve-online-uses-observability-ease-migrations
- Nosy Gamer on Quasar/data-driven design: https://nosygamer.blogspot.com/2025/09/eves-carbon-engine-quasar-and-data.html
- Worthplaying on Quasar: https://www.worthplaying.com/article/2021/10/6/news/128717-eve-onlines-implements-quasar-tech-to-accommodate-more-players-bigger-battles/
- Carbon open-sourcing: https://gamefromscratch.com/carbon-game-engine-open-sourced/ ; https://www.gamingonlinux.com/2026/07/carbon-engine-framework-powering-eve-online-is-now-open-source/ ; https://massivelyop.com/2026/07/01/eve-onlines-fenris-creations-just-open-sourced-the-carbon-engine-framework-its-built-on/ ; https://vgtimes.com/gaming-news/159966-eve-onlines-carbon-engine-goes-open-source-on-github-key-economy-systems-remain-closed.html
- Python 3: https://simonwillison.net/2026/Aug/25/eve-online-move-to-python-3/ ; https://talkpython.fm/episodes/show/564/eve-online-departs-for-python-3
- Stackless in EVE slides: https://www.slideshare.net/slideshow/stackless-python-in-eve/64850 ; https://slideplayer.com/slide/8123277/
- Fuzzwork SDE: https://www.fuzzwork.co.uk/2021/07/17/understanding-the-eve-online-sde-1/ ; ESI Dogma: https://docs.esi.evetech.net/docs/dogma.html
