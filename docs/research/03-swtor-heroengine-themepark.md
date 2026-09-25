# 03 — SWTOR, HeroEngine, and Story-Driven / Themepark Sci-Fi MMOs

*Research note for the engine/editor/backend design team. Compiled 2026-09-25.*

> **Method and confidence.** Direct page fetches were blocked by the network egress proxy for almost every
> relevant domain, including hewiki.heroengine.com, gamedeveloper.com, swtor.com, venturebeat.com and fandom.
> Facts marked with a citation come from search-engine extracts of the cited URLs. A few come from GitHub
> pages that were fetched directly. Statements tagged **[bk]** come from background knowledge that I could not
> check against a fetched source in this session. Treat [bk] items as likely correct but verify them before
> quoting them externally. Wikipedia was not used.

---

## 1. Overview

Star Wars: The Old Republic (SWTOR, BioWare Austin, launched December 2011) is still the clearest large-scale
example of a **story-first MMO**. It had eight fully voiced class stories, cinematic dialogue-wheel
conversations (including multiplayer ones), companions, instanced "flashpoints", and phased personal story
spaces. It was built on an early, source-licensed fork of **HeroEngine** (Simutronics, later Idea Fabrik).
HeroEngine's selling point was *massively collaborative, live, in-world game building*: developers log into a
running server with the HeroBlade editor and build the world together while it runs, with no nightly build.

For us, the key takeaways are:

1. **HeroEngine's architecture still looks modern and fits our stack.** Its schema-first object model, live
   server-authoritative editing, edit and play instances, cluster-wide script hot-push, message router and asset
   streaming map onto almost every part of our plan (C++ zone servers, Go, NATS, Postgres).
2. **SWTOR's pain came from how it adopted the engine, not from the engine's ideas.** BioWare forked an
   unfinished beta and built the engine and the game at the same time. It then shipped a DX9 client that stayed
   32-bit until 2023. The DX12 port, using Frostbite components, is still in progress, and the **UI** is the
   hardest part.
3. **Story at MMO scale is a content-pipeline problem.** SWTOR has more than 200k VO lines, more than 260k data
   objects and more than 558k strings. Dialogue, VO, localization, phasing and quest state have to be one data
   system.
4. **Instancing is the themepark scaling tool, and large battles are its weak spot** (Ilum). That matters for an
   engine that also targets EVE and Star Citizen style play.

---

## 2. HeroEngine Architecture and Live Collaborative Editing

### 2.1 Lineage

- HeroEngine was built by Simutronics for its own MMO, *Hero's Journey*. At GDC 2007 it announced three
  licensees ([Game Developer](https://www.gamedeveloper.com/game-platforms/gdc-simutronics-heroengine-gets-three-licensees)).
  Simutronics described it as offering "simultaneous live, online, real time editing of MMO worlds", which it
  called "massively collaborative game building".
- **BioWare was the first licensee.** Simutronics' Neil Harris said BioWare "beat our door down" before
  HeroEngine was even meant to be sold. The deal was made public in 2008, about two years after it was signed
  ([Ten Ton Hammer](https://www.tentonhammer.com/articles/exclusive-swtor-heroengine-interview-instant-world-creation),
  [Engadget 2008](https://www.engadget.com/2008-12-10-bioware-chooses-heroengine-for-swtor-development.html)).
- **ZeniMax Online Studios** licensed HeroEngine in 2007 to prototype what became *The Elder Scrolls Online*
  ([Game Developer](https://www.gamedeveloper.com/game-platforms/zenimax-online-studios-licenses-simutronics-heroengine-for-new-mmo)).
  ZeniMax later shipped on its own engine [bk].
- After SWTOR launched, HeroEngine passed 5,000 licensees, mostly indie and hobby users
  ([Game Developer](https://www.gamedeveloper.com/business/-em-swtor-em-success-helps-heroengine-surpass-5-000-licensees)).
  By 2012 VentureBeat described it as owned by Maryland-based Idea Fabrik
  ([VentureBeat](https://venturebeat.com/2012/01/21/heroengine-is-the-unsung-platform-behind-star-wars-the-old-republic/)).

### 2.2 Server process model

The HeroEngine wiki describes a server made of many cooperating daemons
([Process](http://hewiki.heroengine.com/wiki/Process), [World Server](http://wiki.heroengine.com/wiki/World_server),
[Area Server](http://hewiki.heroengine.com/wiki/Area_Server)). All of them are "FireStorm daemons", started by
Master Control through a per-machine "Fireup Daemon". Nearly every one of them has a counterpart in our planned
stack:

| HeroEngine process | Role (per HE wiki) | Our equivalent |
|---|---|---|
| **World Server** | Defines a whole shard ("a universe"); allows or refuses moves between areas; creates and deletes area instances | Go orchestration/world service |
| **Area Server** | Runs an area, or part of one; started and stopped by the World Server on any machine in the server group, depending on load | C++ zone/cell server |
| **Dude Server** (+ Dude Manager Director) | Holds player connections and opens connections to the servers each player needs | Edge gateway / connection proxy |
| **Repository Server** (+ Director) | Serves asset requests from clients and talks to the Repository database; scaled by load | Content-addressed asset service + CDN |
| **Authenticator** | Checks accounts, passwords and subscriptions | Go auth service |
| **ID Server** | Hands out IDs to Dude and Area servers | Snowflake-style ID allocation |
| **Post Office Router** | Carries messages between processes | NATS |
| **Stat Center** | Collects statistics from every process | Metrics/telemetry pipeline |
| **Physics Server / Pathmaker** (+ Director) | Physics calculations and pathfinding data for areas | Navmesh bake service; Jolt runs inside the zone server |
| **HeroScript Compiler Server** (server and client versions) | Compiles scripts, then tells every server to load the new code; client bytecode is pushed to the repository | Script compile/validate service |
| **Master Control + Fireup Daemon** | Cluster process supervisor | Kubernetes/Nomad or our own agent |

The design lesson is that **orchestration (World Server) is its own authority, separate from simulation
(Area Servers)**. Area processes are disposable, and the orchestrator places them on hardware by load.

### 2.3 Areas and instances: the edit instance

- "Each area in a world can have multiple instances (copies) of it running at the same time. One of these is
  special, an **Edit instance** which can be changed. The rest are **Play instances**." The edit instance is
  always instance #0, play-instance numbers are assigned dynamically, and **"Play instances do not persist
  changes, only EDIT instances"** ([Area FAQ](http://hewiki.heroengine.com/wiki/Area_FAQ),
  [Area Server](http://hewiki.heroengine.com/wiki/Area_Server)).
- Each area can have unlimited play instances but only one edit instance.
- This is the foundation of live collaborative editing. Developers edit instance #0 together while testers and
  players are in play instances. New play instances pick up the edited content.

### 2.4 Data model: DOM, GOM, and a DDL kept separate from script

- In HeroEngine, "the data object model (classes, fields, enums, etc.) are defined **independently of the
  scripting language**". The wiki credits this separation for the engine's "real-time collaborative
  flexibility" ([HSL for Programmers](http://hewiki.heroengine.com/wiki/HSL_for_Programmers)).
- **The GOM (Game Object Model) has two parts:** the DOM definitions (classes and fields) and the data that uses
  them (nodes and variables). **Classes and fields can only be created or changed through the CLI or the DOM
  Editor, never from HeroScript** ([GOM](http://hewiki.heroengine.com/wiki/GOM),
  [DOM Editor](http://hewiki.heroengine.com/wiki/DOM_Editor)). The schema is a governed, tool-edited artifact.
- **Replication** keeps copies of nodes in destination processes and sends change notifications. It gives "very
  sophisticated and fine grained control of the replication of classes and their member fields"
  ([Replication](http://hewiki.heroengine.com/wiki/Replication)).
- **Persistence is hierarchical.** Persisted nodes hang off a *root node* (account, character, area, or an
  *arbitrary root node*) through "hard associations". When a root loads, everything hard-associated with it
  loads too. Arbitrary root nodes can load lazily, in a different GOM (process), and can belong to an account or
  a character ([Arbitrary Root Node](http://hewiki.heroengine.com/wiki/Arbitrary_Root_Node)). This is the
  document/aggregate model, the same one we would use with Postgres JSONB aggregates or per-root tables.
- **This DNA survives in SWTOR today.** An open-source SWTOR datamining tool parses "GOM binary" objects
  (header + fully-qualified name + payload) from the game's `.tor` archives. The FQN prefixes include `abl`
  (abilities), `qst` (quests), `npc`, `itm` and `tal`. The tool extracts "260k+ game objects" and "558k+
  localized strings" ([ssilvius/kessel](https://github.com/ssilvius/kessel)). Fifteen years on, SWTOR content is
  still schema-typed GOM nodes addressed by hierarchical names.

### 2.5 HeroScript (HSL) and hot reload

- HSL is a compiled, statically typed scripting language [bk: static typing]. Server scripts go to the
  **HeroScript Compiler Server**, and "after a successful compile, this server informs all other servers to load
  the new script code". Client scripts are compiled by a separate client compiler server and pushed into the
  client repository ([Process](http://hewiki.heroengine.com/wiki/Process)).
- The wiki says there is "essentially zero compile-link-deploy-restart time" and that you can "develop your game
  without ever restarting; content, data definitions, game data, and game logic all update dynamically"
  ([HSL for Programmers](http://hewiki.heroengine.com/wiki/HSL_for_programmers),
  [HeroScript](http://hewiki.heroengine.com/wiki/HeroScript)).
- Native C++ plugins can also be loaded and unloaded without restarting the client or the server
  ([HeroScript Extension Plugin](http://hewiki.heroengine.com/wiki/HeroScript_Extension_Plugin)).
- HeroEngine shipped an optional starter gameplay layer written in HSL, the "MMO Foundation Framework" / "Clean
  Engine", which licensees adapted
  ([Adapting Clean Engine](http://hewiki.heroengine.com/wiki/Adapting_Clean_Engine)).

### 2.6 Spatial awareness and the seamless world

- The `$SPATIALAWARENESS_AREA` system node "is in charge of managing the introduction of players and NPCs to a
  client". It gives the Dude Server positional information "for use in prioritizing and shaping traffic via
  Replication", but "it is not AI" ([Spatial Awareness System](http://hewiki.heroengine.com/wiki/Spatial_Awareness_System)).
  In other words, interest management is exposed as a scriptable system with hooks, not hard-coded in C++. The
  wiki notes it replaced behaviour that used to live in the C++ "Character Command Center".
- **Seamless World 1.0** stitches area instances into what looks like continuous terrain. It does this in four
  steps: **preload** the destination's assets, **proxy** entities near the boundary, **transfer** the player's
  node hierarchy, and **transition** (usually through a "transition piece" plus a trigger)
  ([Seamless world](http://hewiki.heroengine.com/wiki/Seamless_world),
  [Seamless Area Link Tutorial](http://hewiki.heroengine.com/wiki/Seamless_Area_Link_Tutorial)).
  **Seamless 2.0** has the client render the linked areas at the same time as the current one, where 1.0 used
  "a loading corridor between two areas" ([Seamless World 2.0](http://hewiki.heroengine.com/wiki/Seamless_World_2.0)).
  Seamless area links register spatial entities that decide when to launch, preload, proxy and hand off. This is
  an early, instance-granularity version of server meshing.

### 2.7 The repository and asset streaming

- Artists use a separate **Repository Browser** to upload assets from their machines to the server-side
  repository. HeroEngine can then read them immediately
  ([Repository Browser](http://hewiki.heroengine.com/wiki/Repository_Browser),
  [Repository](http://hewiki.heroengine.com/wiki/Repository)).
- Clients keep a **Local Repository Cache (LRC)**. If a file is missing or outdated, "the server automatically
  streams the file on demand". Three install modes are supported: *streaming* (about a 10 MB client, everything
  else streamed), *full*, and *hybrid* (an optimized subset preinstalled)
  ([Local Repository Cache](http://wiki.heroengine.com/wiki/Local_Repository_Cache)).

### 2.8 The HeroBlade workflow

- HeroBlade is the single front end for "area creation, designing data and game objects, creating special
  effects, implementing GUIs, script editing, and more" ([HeroBlade](http://wiki.heroengine.com/wiki/HeroBlade)).
- Collaboration is spatial and live. One developer "can be creating a house and the entities inside, while
  another works on the landscaping and terrain around it. Each sees the other's work in real time." Developers can
  pin **notes directly onto in-game locations and attach them to tasks** for other developers
  ([Game Developer 2007](https://www.gamedeveloper.com/game-platforms/gdc-simutronics-heroengine-gets-three-licensees),
  [VentureBeat](https://venturebeat.com/2012/01/21/heroengine-is-the-unsung-platform-behind-star-wars-the-old-republic/)).
- Teams in different locations "build the world while simultaneously playing the game live with changes that
  take effect instantaneously", with "no nightly builds"
  ([Ten Ton Hammer](https://www.tentonhammer.com/articles/exclusive-swtor-heroengine-interview-instant-world-creation)).

### 2.9 Assessment

**What to copy:** everything in §2.2–2.8 (see §8, P0-1 to P0-7).

**What to avoid or improve:**

- **Content lived in a live database, not a versioned tree.** Branching, code review, diffing, rollback and
  keeping content in step with code versions are hard in a world where everything is live. HeroEngine does
  version scripts and the DOM, but world content is effectively whatever is in the current database [bk,
  inference]. Studios need release branches and hotfix branches.
- **"X players for any value of X".** The scalability page says capacity depends on each game's per-player
  budget of CPU, RAM, database I/O and persisted data size
  ([Scalability](http://hewiki.heroengine.com/wiki/Scalability_and_Building_For_Massive_Multiplayer_Audiences)).
  That is true, but it means the engine will not stop you from blowing the budget. We need enforced budgets and
  telemetry.
- **A proprietary scripting language** limits hiring and tooling. The value was in the architecture around HSL,
  not in HSL itself.

---

## 3. SWTOR Tech and Server Architecture

### 3.1 How the fork happened

- BioWare licensed a **pre-release HeroEngine with source access** around 2006 and modified it heavily.
  Community and press summaries say the changes went so far that HeroEngine's owners told BioWare it was on its
  own ([MMORPG.com forum summary](https://forums.mmorpg.com/discussion/385736/wait-hero-engine); lower
  reliability).
- Neil Harris, HeroEngine's President and COO, later wrote about the state of the code at the time of the sale:
  "There are whole sections of code that is only roughed in and not optimized for performance or security. And
  there are very few comments and very little documentation." He quoted BioWare's Gordon Walton as replying
  "We are going to have tons of engineers. We can finish it ourselves. We're going to want to modify your source
  code for our special project anyway." (Harris's blog, as quoted at
  [MMO-Champion](https://www.mmo-champion.com/threads/1070748-President-and-COO-of-HeroEngine-comments-on-SWTOR-coding!)
  and the [SWTOR forum archive](http://www.swtor.com/community/showthread.php?t=232960).)
- SWTOR took about six years, up to 800 developers, and an estimated $200M
  ([VentureBeat](https://venturebeat.com/2012/01/21/heroengine-is-the-unsung-platform-behind-star-wars-the-old-republic/)).
  Commentators consistently name the root problem as **building the engine and the MMO at the same time**.

### 3.2 The client technology arc

- **DirectX 9 through at least spring 2026.** The live client has been on D3D9 from launch
  ([Steam graphics guide](https://steamcommunity.com/sharedfiles/filedetails/?id=2301408368)). Early complaints
  described performance that was "extremely inconsistent across machines", with high-end PCs sometimes
  performing worse than weaker ones. Promised high-resolution textures were cut back at launch
  ([MMO-Champion thread](https://www.mmo-champion.com/threads/1453025-What-s-Wrong-with-the-Hero-Engine)).
- **64-bit only in March 2023.** The 64-bit client shipped with Game Update 7.2.1 on 28 March 2023. It replaced
  the 32-bit client completely, as a roughly 35 GB download and roughly 48 GB installed
  ([VULKK](https://vulkk.com/2023/04/02/swtor-64-bit-everything-you-need-to-know/),
  [7.2.1 notes](https://vulkk.com/2023/03/27/swtor-7-2-1-changes-overview-and-patch-notes/)). The old
  32-bit client's address-space crashes ("the 2GB crash") had been a complaint for more than a decade.
- **DirectX 12 with Frostbite components, 2025–26.** Broadsword, which has run SWTOR since 2023 [bk], says
  "little of the original [HeroEngine] code remains… it's basically now the SWTOR Engine". The team is
  integrating **rendering components from EA's Frostbite** without converting the game to Frostbite, because
  the content depends on the SWTOR engine. The team started from a stripped-down baseline (objects drawn with no
  textures) so it could remove DirectX 9 assumptions. **Converting the UI has become the main blocker** before a
  technical alpha, because "there was not a direct upgrade path for the UI"
  ([SWTOR DX12 Spring 2026 update](https://www.swtor.com/info/news/[news-category]/20260331),
  [Swtorista](https://swtorista.com/articles/swtor-directx-12-progress-update-frostbite-engine-integration/),
  [Today in TOR](https://todayintor.com/2026/04/01/swtor-directx-12-update/)).

### 3.3 Server topology and scaling

- **Shards with instanced planets.** Each server (shard) runs each planet as one or more *instances*, and the
  orchestrator opens more as population caps are reached. Players can switch instances from a UI drop-down.
  Planet caps were raised right after launch
  ([forum: planet cap raised](https://forums.swtor.com/topic/94654-planet-instances-player-cap-raised)). At
  launch, shard caps were around 3,600 concurrent players, with queues of 1,000 or more on popular servers
  ([forum: 3,600 cap](https://forums.swtor.com/topic/114517-current-final-server-population-cap-3600-players/),
  [Prima](https://primagames.com/news/swtor-bioware-warns-against-creating-characters-on)). This follows
  HeroEngine's world-server-and-area-instances model directly.
- **Consolidation.** Server merges in late 2017 cut the list to a handful of shards and replaced PvE and PvP
  *server types* with a PvP *instance* of each planet on every server. In 2026 the roster is Star Forge and Satele
  Shan (NA, Virginia), Darth Malgus, Tulak Hord and The Leviathan (EU, Dublin), and Shae Vizla (APAC)
  ([Server Directory](https://swtor.fandom.com/wiki/Server_Directory)).
- **Cross-faction, not cross-server.** Game Update 5.9.2 made all Warzone and Starfighter queues
  cross-faction and added role balancing (at most two tanks or healers per team)
  ([forum](https://forums.swtor.com/topic/867167-upcoming-matchmaking-changes/)). Players still ask for
  cross-server group finding
  ([forum](https://forums.swtor.com/topic/809503-cross-server-group-finder/)). In 2026, critics argue that
  uneven shard populations need **regional megaservers plus a revamped character-name system**, because name
  collisions block merges ([VULKK 2026](https://vulkk.com/2026/01/17/broadsword-is-leaving-swtors-satele-shan-server-behind/)).
- **Cloud move in 2023.** BioWare announced the move to AWS in March 2023. It tested with a new APAC server,
  Shae Vizla, from 3 to 18 April 2023. The first live migration was The Leviathan on 15 August 2023, with about
  six hours of downtime. The stated goals were backend health and stability and freeing up resources
  ([SWTOR news 2023-08-11](https://www.swtor.com/info/news/20230811),
  [Massively OP](https://massivelyop.com/2023/03/30/star-wars-the-old-republic-plans-to-move-its-servers-over-to-amazon-web-services/),
  [VULKK](https://vulkk.com/2023/08/12/swtor-update-7-3-1-and-imminent-move-to-cloud-announced/)).
  I could not find public documentation in this session of data-centre moves before 2023, such as any in 2019.

### 3.4 Performance problems

- **Ilum open-world PvP (patches 1.1–1.2, 2012).** The zone lagged badly with about 25 players and became "a
  slideshow" with 30 or more. After 1.1, zerg-camping made it worse. BioWare published a dedicated post, "Ilum
  PvP issues", and eventually abandoned Ilum as an open-world PvP showcase
  ([SWTOR blog](https://www.swtor.com/blog/ilum-pvp-issues),
  [forum thread](https://forums.swtor.com/topic/169895-update-on-ilum-open-world-pvp-issues/)).
  The large-battle pressure hit the client (per-character draw and animation cost) and the server
  (ability processing and replication fan-out) at the same time.
- Crowded hubs such as the Fleet and big group fights stayed frame-rate hotspots for years. That is typical of a
  D3D9-era renderer that is CPU and draw-call bound, with a single-threaded 32-bit client [bk].

---

## 4. Story, Dialogue and Cinematics Tooling

### 4.1 What SWTOR shipped

- **Eight class stories.** Jedi Knight, Jedi Consular, Smuggler, Trooper, Sith Warrior, Sith Inquisitor, Bounty
  Hunter and Imperial Agent each have a prologue and three acts. BioWare pitched each one as comparable in size
  to a single-player BioWare RPG [bk]. On top of those sit shared planetary stories and side missions.
- **Cinematic conversations** use the BioWare dialogue wheel (short paraphrase leads to a full voiced line).
  Choices carry Light/Dark alignment and affect companions. Originally this was "affection", which became
  "influence" in the 2015 *Knights of the Fallen Empire* rework [bk].
- **A fully voiced player character** in every class, with male and female versions, plus full NPC VO. SWTOR
  holds the Guinness World Record for "Largest Entertainment Voice Over Project": more than 200,000 recorded
  lines by several hundred actors
  ([PC Gamer](https://www.pcgamer.com/star-wars-the-old-republic-scoops-guinness-world-record-for-voice-acting/),
  [GameFront](https://www.gamefront.com/games/gamingtoday/article/swtor-wins-guinness-world-record-for-voice-acting)).
  The game launched with English, French and German. Localizing that volume was the subject of a GDC
  Localization Summit talk by Gordon Walton and Ian Mitchell
  ([MMORPG.com summary](https://forums.mmorpg.com/discussion/308965/gdc-localisation-in-swtor)).
- **Alien-language NPCs** (Huttese and others) reuse a limited pool of alien VO lines with subtitles. This is a
  deliberate way to control cost [bk].
- **Companions.** Each class has several companions, with their own conversations, romances, and roles in crew
  skills (crafting and gathering missions). They live on the player's personal starship, which is itself an
  instance [bk].

### 4.2 Group (multiplayer) conversations

This is SWTOR's most unusual feature, and our engine would need it for group story content:

- Every group member within range joins the same cinematic. Each picks a response. A random roll decides whose
  choice is played, and that player's own character speaks the line
  ([Fandom: Social Points](https://swtor-archive.fandom.com/wiki/Social_Points),
  [Game Developer: The Making of SWTOR](https://www.gamedeveloper.com/business/the-making-of-i-star-wars-the-old-republic-i-)).
- Outcomes are **per player where it matters**. Each player earns the alignment points for *their own* pick,
  whoever wins the roll [bk]. **Social points** reward taking part: one point per roll, scaled up to four by
  group size, and two points for winning, scaled up to eight. They are paid out when the mission is turned in
  ([Fandom: Social Points](https://swtor-archive.fandom.com/wiki/Social_Points)).
- The runtime therefore needs to (a) synchronise one cinematic across N clients, (b) collect choices with a
  timeout, (c) resolve them on the server, (d) swap in the winner's character as speaker, voice and animation
  set, and (e) apply per-player side effects. Skipping lines in groups needs a synchronisation rule, for example
  a line advances once everyone has skipped it [bk].

### 4.3 Cinematic production approach

BioWare's approach in that era was to generate each conversation's staging automatically, then have cinematic
designers polish the important scenes. Staging here means speaker placement, camera shots, gestures and
lip-sync. It was driven by line metadata such as speaker, listener, emotion and intent [bk; based on BioWare's
Dragon Age and Mass Effect toolsets]. At about 200k lines, hand-keying every shot is impossible, and automation
decides the baseline quality. The later **chapter-based Knights of the Fallen Empire** (2015) moved from eight
parallel class stories to one shared storyline with class-flavoured lines. That was largely a response to the
cost of producing class stories in parallel [bk].

### 4.4 What this requires from the engine

Dialogue nodes, conditions, effects, speaker roles, VO references, localized strings and staging metadata need
to be one data model. SWTOR stores them as GOM nodes plus `.stb` string tables with a template grammar such as
`<<1[%d seconds...]>>` ([kessel](https://github.com/ssilvius/kessel)). The tooling this implies is listed in
§8 (P0-11 and P1-1 to P1-4).

---

## 5. Instancing and Phasing

SWTOR combines several kinds of instance [bk unless cited]:

| Layer | Scope | Mechanism |
|---|---|---|
| Planet instances | Capacity overflow; switchable; PvP versions | Area instances opened by the orchestrator ([forum](https://forums.swtor.com/topic/94654-planet-instances-player-cap-raised)) |
| Class-story phases | One player plus their group, if eligible | Colour-coded force-field doors (green: you can enter; red: you cannot) leading into a personal instance |
| Heroic areas | Open-world group content | Shared world with difficulty gating (some later converted to phased or scaled versions) |
| Flashpoints | 4 players | Group instances with group conversations; later solo and scaled modes |
| Operations | 8 or 16 players | Raid instances with lockouts |
| Warzones / GSF | Matched PvP | Match instances (cross-faction since 5.9.2) |
| Personal starship | One player (plus group guests) | Personal instance serving as a hub, with galaxy-map travel and companion scenes |
| Strongholds (2014) | Player housing | Persistent per-player instance with hook-based decoration |

HeroEngine supports this out of the box: any area can have many play instances created on demand by the World
Server. **The door is a design convention that hides the transfer between instances.** The lesson for an engine
is that phases and instances must be first-class concepts in interest management and orchestration, not added
later in script. Two further points:

- The rules for sharing a phase with a party are complicated: who may enter, whose story state applies, and what
  happens when progress differs. They belong in data, and the editor needs a way to debug a scene as a player
  in a given state.
- Personal phases fragment the world. Social MMOs such as SWG and EVE need most content in shared space, so
  phasing should be used sparingly and be visible to designers.

---

## 6. Other Themepark Sci-Fi MMOs

### 6.1 Star Trek Online (Cryptic, 2010) and the Foundry

- **Engine.** STO runs on Cryptic's in-house engine, shared with Champions Online and Neverwinter. Content is
  built from instanced maps, each with multiple instances, plus "sector space" travel maps. Space combat
  (firing arcs, shield facings) and ground combat are separate combat models [bk].
- **The Foundry** was an in-game mission editor for players [bk]. It had a *story* tab (an ordered chain of
  objectives, dialogue and triggers), a *map* editor (NPCs, contacts, objects, trigger volumes on ground and space
  maps), a *dialogue tree* editor, and an NPC costume editor. Missions went through a publish and review step,
  were rated by players, and were listed in an in-game browser. Neverwinter used the same system. Fan archives
  of Foundry missions still exist
  ([GitHub](https://github.com/search?q=%22Star+Trek+Online%22+foundry&type=repositories)).
- **Outcome.** Cryptic **retired the Foundry in STO and Neverwinter in 2019**, citing the technical cost of
  keeping it working as the engine changed. Neverwinter Foundry quests had also been exploited as XP farms [bk].
  **Lesson:** UGC needs a *stable, versioned content format* with automatic migration, a *sandboxed* runtime,
  and reward rules built against exploits from the start.

### 6.2 Anarchy Online (Funcom, 2001)

- AO was an early user of **on-demand instanced missions** [bk]. Mission terminals generated randomised
  objectives (find an item, kill a target, use an object) in instanced dungeons assembled from tile sets.
  Sliders let players bias the mission type and rewards. This is cheap content multiplication from templates
  and modular kits, which suits EVE-style missions and SWG-style mission terminals.
- AO had a notoriously troubled launch, and its promised engine upgrade slipped for years [bk], another example
  of how hard it is to replace an engine under a live MMO. Emulators such as
  [CellAO](https://github.com/CellAO/CellAO) split the backend into zone, login and chat services.

### 6.3 WildStar (Carbine / NCSoft, 2014–2018)

- **Telegraph combat.** Every ability, from players and enemies, draws its area of effect on the ground, and
  hits are resolved against those shapes. That needs shape primitives shared by client and server, deterministic
  server hit tests, and latency-compensated decals [bk].
- **Paths** (Soldier, Settler, Scientist, Explorer) layer per-player objectives into the *shared* zone. This is
  **additive phasing**: content is filtered per player without moving anyone to another instance [bk].
- **Housing** on floating sky plots used "FABkit" slots and **free-form decor placement** with move, rotate and
  scale gizmos. It is widely considered the best housing of its generation [bk]. The tool requirement is a
  **player-facing subset of the level editor**, validated on the server and held to budgets.
- WildStar consolidated into regional megaservers around its 2015 free-to-play relaunch and closed in
  November 2018. CPU-bound client performance is often cited among its problems [bk]. The
  [NexusForever](https://github.com/NexusForever/NexusForever) emulator rebuilds the backend as services
  connected by a message broker.

### 6.4 The Elder Scrolls Online (ZeniMax Online, 2014) — the megaserver

- ZeniMax **prototyped on HeroEngine in 2007**
  ([Game Developer](https://www.gamedeveloper.com/game-platforms/zenimax-online-studios-licenses-simutronics-heroengine-for-new-mmo))
  and then shipped on its own engine [bk].
- **Megaserver:** one logical world per platform and region (NA and EU). Each zone runs many instances, and a
  placement service puts players into instances using social affinity: group, friends, guildmates, language,
  and play style [bk]. Quest-state phasing hides players whose world state differs. Cyrodiil PvP runs as
  separate **campaigns** with population caps [bk].
- **Lesson:** the megaserver is mostly *identity plus a placement policy plus instance orchestration*, not one
  huge simulation. It needs globally unique identity (ESO uses account-wide @handles [bk]), which SWTOR's
  per-server names lack (§3.3). ESO's persistent Cyrodiil lag in large fights mirrors Ilum: **megaservers solve
  social density, not battle density.**

---

## 7. Lessons and Pitfalls

1. **Do not fork an unfinished engine and build the game on it at the same time.** SWTOR carried that cost for
   15 years. Harden the core runtime in vertical slices, with performance budgets enforced in CI.
2. **Keep the UI independent of the renderer.** In SWTOR's DX12 port, the UI had no upgrade path and became
   the blocker.
3. **Live collaborative editing multiplies productivity, but it needs publishing gates:** explicit edit and play
   instances, promotion steps, and content versions pinned per instance.
4. **Schema-first data is the backbone at MMO scale.** SWTOR still ships HeroEngine-style GOM nodes.
5. **Branching story multiplies VO and testing cost** (8 classes × 2 genders × 3 languages × choices ×
   companions × group play). Coverage tooling is mandatory.
6. **Instances hide scaling limits, but players find the edges** (Ilum, Cyrodiil, crowded hubs). Budget for
   large battles from the start.
7. **Build global identity and cross-shard social services from day one.** Name collisions block SWTOR's
   merges, and it never shipped cross-server group finding.
8. **UGC is a product, not a feature flag.** The Foundry died from maintenance cost plus exploits.
9. **Minigame-style space was a stopgap.** SWTOR shipped an on-rails shooter and added 12v12 *Galactic
   Starfighter* about two years later [bk]. For EVE or Star Citizen scope, space has to share the same physics,
   netcode and large-coordinate world as ground play.
10. **Replacing the engine under a live MMO takes years** (SWTOR's DX12 port, AO's engine upgrade). Start on
    modern foundations: Vulkan 1.3, 64-bit, job system, bindless resources.

---

## 8. Requirements for Our Engine

Priority key: **P0** = needed for the first vertical slice and architecture that is hard to retrofit.
**P1** = needed before content production scales. **P2** = differentiators and later additions.

### 8.1 Stack flags from this research

- **LuaJIT vs. a typed, sandboxed scripting language.** HSL's static typing and central compile service helped a
  studio of hundreds. LuaJIT is dynamically typed, stuck at Lua 5.1 semantics, and has limited upstream
  maintenance. It is **not designed as a security sandbox for player-authored code** (see UGC, §6.1).
  *Recommendation:* evaluate **Luau** (gradual types, built-in sandboxing, actively maintained) against LuaJIT
  plus an offline type-checker. Either way, put a compile and validation service in front of all script
  deployment, as HeroEngine's compiler server did.
- **Dear ImGui editor.** Fine for the world editor and technical tools. Writers, localization and VO teams need
  **web-based tools** (Go backend, browser UI) that work on the same content store. They should never have to
  install the 3D editor to edit dialogue.
- **Postgres, NATS and Go** map cleanly onto HeroEngine's repository, Post Office Router and service processes.
  Add three services the current plan does not name: a **content service** (versioned object store), an
  **instance orchestrator** (the World Server role), and a **compile/validate service**.

### 8.2 P0 — architecture and core tools

| ID | Requirement | Why / source |
|---|---|---|
| P0-1 | **Schema-first object model (DDL).** One schema language defines classes, fields and enums. Each field is annotated with persistence, replication (scope, rate, owner-only), authority, edit permissions, localizable flag and editor hints. The schema generates C++ structs and serializers, Lua and Go types, Postgres migrations, and editor property panels. Schema changes can be applied to running dev servers through versioned migrations. | HeroEngine DOM/GOM kept separate from script; SWTOR still runs on GOM (§2.4) |
| P0-2 | **Editor as a networked client.** The editor connects to a running *edit instance* of a zone or cell. Every edit is a server-validated operation broadcast to all connected editors, with presence (who is where and selecting what), soft locks per object or region, per-user undo, and an "enter play instance" button with no restart. | HeroBlade live collaborative editing (§2.8) |
| P0-3 | **Edit, play and publish separation.** Only edit instances persist changes. Play instances start from a *pinned content version*. Promotion runs from dev-edit to staging to live, with diffs, sign-off and rollback. Live servers are never edited directly. | HE edit instance #0 (§2.3) plus the lesson in §7.3 |
| P0-4 | **Versioned content store.** The authoritative object store (Postgres) keeps changeset history, branches and labels (release and hotfix), a diffable text export mirrored to git for review and CI, and content-addressed binary assets. | Fixes HeroEngine's live-database-only weakness (§2.9) |
| P0-5 | **Hot reload of everything in dev:** scripts, data, shaders, materials, UI and assets. A compile/validate service type-checks and lints, then pushes to every server and client over NATS. No restart. | HSL compiler server and plugin hot-load (§2.5) |
| P0-6 | **Instances, layers and phases in the zone-server core.** Every entity has a visibility scope (world, layer, instance, phase set, party, player). Interest management and replication respect it. The orchestrator spawns capacity layers, personal and group story instances, and match instances, targeting under 2 s to start a small story instance. | SWTOR planet instances and phases, ESO layering (§3.3, §5, §6.4) |
| P0-7 | **Scriptable interest-management and transfer framework.** Priority-based replication with designer hooks (like Spatial Awareness), plus preload, proxy, transfer and transition between cells and instances, with no loading screen within a region. | HE Spatial Awareness and Seamless World 1.0/2.0 (§2.6) |
| P0-8 | **Global identity and shard-agnostic services.** Globally unique account and character IDs, a global display-name scheme (handle plus discriminator), and cross-shard group finder, chat, guilds, mail and market from the start. Megaserver placement policy: friends, guild, language, play style. | SWTOR merge and name problems, ESO megaserver (§3.3, §6.4) |
| P0-9 | **Render hardware interface and UI decoupling.** 64-bit only. A render hardware interface over Vulkan 1.3 (backend swappable). The UI renderer uses only that interface. No content format may depend on a specific graphics API. | SWTOR's D3D9 lock-in and UI port blocker (§3.2) |
| P0-10 | **Large-battle budgets and load-test harness.** Headless bot swarms (hundreds to thousands of clients), per-zone CPU, RAM, database-I/O and bandwidth budgets per player, automatic degradation (replication LOD, time dilation), and performance regression gates in CI. | Ilum, Cyrodiil, the HE scalability page (§3.4, §2.9) |
| P0-11 | **Server-authoritative conversation runtime.** Dialogue graph with conditions and effects, speaker *roles* bound at runtime (for example, a player-character slot), and multiplayer sessions with choice collection, timeouts and a pluggable resolver (roll, leader or vote). Per-player side effects, skip synchronisation, and handling of players who disconnect. | SWTOR group conversations (§4.2) |

### 8.3 P1 — needed before content production scales

| ID | Requirement | Why / source |
|---|---|---|
| P1-1 | **Conversation and cinematic editor.** A graph view *and* a screenplay-style text view that round-trip. **Auto-staging** generates cameras, blocking, look-ats, gestures and lip-sync from line metadata (speaker, listener, emotion, intent). A timeline override for important scenes. Live preview in the editor with placeholder TTS. | SWTOR scale, BioWare auto-cinematic approach (§4.3) |
| P1-2 | **VO and localization pipeline.** Stable line IDs; state tracking (written, locked, recorded, implemented); recording-script export per actor and language; batch lip-sync; pools of alien-language lines; subtitles; ICU-style string tables with gender and plural grammar; pseudo-localization; tracking of text length and VO timing. | 200k+ lines, three launch languages, 558k strings (§4.1) |
| P1-3 | **Quest and phase authoring with a "view as state" debugger.** The editor shows which entities exist in which phase or quest state. Designers can simulate a player with any flags, class or companion set, and see party phase-sharing results. | SWTOR class phases and doors (§5) |
| P1-4 | **Story coverage analysis.** Reports on unreachable nodes, conditions that are never true, missing VO or translations, and branch combinations untested per class, gender and language. Automated playthrough bots for dialogue graphs. | Combinatorial burden (§7.5) |
| P1-5 | **In-world annotations linked to tasks.** Notes and screenshots pinned to world positions or objects, synced with the issue tracker and shown as editor overlays. | HeroBlade notes attached to tasks (§2.8) |
| P1-6 | **Streaming and hybrid installs in the launcher.** A small bootstrap install, content-addressed chunks, priority streaming driven by the destination zone, background prefetch, and delta patching. | HE repository streaming install modes (§2.7) |
| P1-7 | **Companion and relationship system in data.** Influence or affinity, availability across story branches, companion conversation triggers, combat role presets, and away missions (crew skills). | SWTOR companions (§4.1) |
| P1-8 | **Group-content framework.** Templates for flashpoints and operations (4, 8, 16 players) with difficulty tiers, level sync and bolster, lockouts, solo and scaled modes, and a cross-shard group finder with role balancing. | SWTOR flashpoints and operations, 5.9.2 matchmaking (§3.3, §5) |
| P1-9 | **Player housing and ship-interior instances.** Persistent per-owner instances; both hook-based and free-form decor placement (a player-facing subset of the editor's gizmos); server validation; budgets for decor count and cost; visitor permissions. | SWTOR strongholds, WildStar housing (§5, §6.3) |
| P1-10 | **Shared ability-shape ("telegraph") system.** Shape primitives shared by client and server, deterministic hit tests on the server, latency-compensated client decals, and authoring tools. | WildStar (§6.3) |
| P1-11 | **Live data hotfixes.** Server-side content and data patches without client patches or downtime, plus feature flags per shard or region. | Live-ops need; HE live-update model |
| P1-12 | **Space as a first-class simulation.** Ship combat and flight run in the same zone-server, physics and replication stack as ground play, with large-world coordinates. Story tools (conversations, phases) also work aboard ships and in space instances. | SWTOR's on-rails space, later GSF, was a separate mode (§7.9) |

### 8.4 P2 — differentiators and later additions

| ID | Requirement | Why / source |
|---|---|---|
| P2-1 | **Player UGC editor (a "Foundry" successor).** A sandboxed subset of the editor and scripting, a versioned UGC format with automatic migrations, moderation and reporting, rating and discovery, and reward rules built against exploits. | STO and Neverwinter Foundry (§6.1) |
| P2-2 | **Procedural mission generator.** Parameterised mission templates with modular interior kits, instanced on demand, with player-tunable parameters. | Anarchy Online (§6.2) |
| P2-3 | **Additive per-player content layers** (path or role objectives) inside shared instances. | WildStar paths (§6.3) |
| P2-4 | **Social progression hooks for group story,** such as social points or reputation for taking part in group conversations. | SWTOR social points (§4.2) |
| P2-5 | **Cinematic capture tools:** photo mode, replay of recorded conversations, and export for marketing. | Story-MMO marketing |

---

## 9. Sources

**HeroEngine wiki** (content retrieved through search-engine extracts; direct fetch was blocked):
- HeroBlade — http://wiki.heroengine.com/wiki/HeroBlade
- Area Server — http://hewiki.heroengine.com/wiki/Area_Server
- Area FAQ — http://hewiki.heroengine.com/wiki/Area_FAQ
- World Server — http://wiki.heroengine.com/wiki/World_server
- Process — http://hewiki.heroengine.com/wiki/Process
- HSL for Programmers — http://hewiki.heroengine.com/wiki/HSL_for_Programmers
- HeroScript — http://hewiki.heroengine.com/wiki/HeroScript
- HeroScript Extension Plugin — http://hewiki.heroengine.com/wiki/HeroScript_Extension_Plugin
- Adapting Clean Engine — http://hewiki.heroengine.com/wiki/Adapting_Clean_Engine
- GOM — http://hewiki.heroengine.com/wiki/GOM
- DOM Editor — http://hewiki.heroengine.com/wiki/DOM_Editor
- Replication — http://hewiki.heroengine.com/wiki/Replication
- Arbitrary Root Node — http://hewiki.heroengine.com/wiki/Arbitrary_Root_Node
- Spatial Awareness System — http://hewiki.heroengine.com/wiki/Spatial_Awareness_System
- Seamless world — http://hewiki.heroengine.com/wiki/Seamless_world
- Seamless World 2.0 — http://hewiki.heroengine.com/wiki/Seamless_World_2.0
- Seamless Area Link Tutorial — http://hewiki.heroengine.com/wiki/Seamless_Area_Link_Tutorial
- Repository — http://hewiki.heroengine.com/wiki/Repository
- Repository Browser — http://hewiki.heroengine.com/wiki/Repository_Browser
- Local Repository Cache — http://wiki.heroengine.com/wiki/Local_Repository_Cache
- Scalability and Building For Massive Multiplayer Audiences — http://hewiki.heroengine.com/wiki/Scalability_and_Building_For_Massive_Multiplayer_Audiences

**HeroEngine and SWTOR press and interviews:**
- Game Developer, "GDC: Simutronics' HeroEngine Gets Three Licensees" — https://www.gamedeveloper.com/game-platforms/gdc-simutronics-heroengine-gets-three-licensees
- Game Developer, "ZeniMax Online Studios Licenses Simutronics HeroEngine For New MMO" — https://www.gamedeveloper.com/game-platforms/zenimax-online-studios-licenses-simutronics-heroengine-for-new-mmo
- Game Developer, "SWTOR success helps HeroEngine surpass 5,000 licensees" — https://www.gamedeveloper.com/business/-em-swtor-em-success-helps-heroengine-surpass-5-000-licensees
- Game Developer, "The Making of Star Wars: The Old Republic" — https://www.gamedeveloper.com/business/the-making-of-i-star-wars-the-old-republic-i-
- VentureBeat, "HeroEngine is the unsung platform behind Star Wars: The Old Republic" (2012) — https://venturebeat.com/2012/01/21/heroengine-is-the-unsung-platform-behind-star-wars-the-old-republic/
- Ten Ton Hammer, "Exclusive SWTOR HeroEngine Interview – Instant World Creation" — https://www.tentonhammer.com/articles/exclusive-swtor-heroengine-interview-instant-world-creation
- Engadget, "BioWare chooses HeroEngine for SWTOR development" (2008) — https://www.engadget.com/2008-12-10-bioware-chooses-heroengine-for-swtor-development.html
- MMO-Champion, quoting Neil Harris's blog — https://www.mmo-champion.com/threads/1070748-President-and-COO-of-HeroEngine-comments-on-SWTOR-coding!
- SWTOR forum archive, same topic — http://www.swtor.com/community/showthread.php?t=232960
- MMORPG.com forums, "Wait. Hero Engine?" (community; lower reliability) — https://forums.mmorpg.com/discussion/385736/wait-hero-engine
- MMO-Champion, "What's Wrong with the Hero Engine?" (community) — https://www.mmo-champion.com/threads/1453025-What-s-Wrong-with-the-Hero-Engine

**SWTOR tech, servers and operations:**
- SWTOR DirectX 12 Spring 2026 Update — https://www.swtor.com/info/news/[news-category]/20260331
- Swtorista, DX12 progress and Frostbite integration — https://swtorista.com/articles/swtor-directx-12-progress-update-frostbite-engine-integration/
- Today in TOR, SWTOR DirectX 12 Update — https://todayintor.com/2026/04/01/swtor-directx-12-update/
- VULKK, SWTOR 64-Bit: Everything you need to know — https://vulkk.com/2023/04/02/swtor-64-bit-everything-you-need-to-know/
- VULKK, 7.2.1 patch notes — https://vulkk.com/2023/03/27/swtor-7-2-1-changes-overview-and-patch-notes/
- Steam guide, SWTOR Graphics and Performance Explained — https://steamcommunity.com/sharedfiles/filedetails/?id=2301408368
- SWTOR news, "SWTOR begins to move servers to the cloud!" — https://www.swtor.com/info/news/20230811
- Massively OP, SWTOR moving to AWS — https://massivelyop.com/2023/03/30/star-wars-the-old-republic-plans-to-move-its-servers-over-to-amazon-web-services/
- VULKK, 7.3.1 and cloud move — https://vulkk.com/2023/08/12/swtor-update-7-3-1-and-imminent-move-to-cloud-announced/
- SWTOR Server Directory (Fandom) — https://swtor.fandom.com/wiki/Server_Directory
- VULKK, regional megaservers and names (2026) — https://vulkk.com/2026/01/17/broadsword-is-leaving-swtors-satele-shan-server-behind/
- SWTOR forums, Upcoming Matchmaking Changes (5.9.2) — https://forums.swtor.com/topic/867167-upcoming-matchmaking-changes/
- SWTOR forums, Cross-Server Group Finder request — https://forums.swtor.com/topic/809503-cross-server-group-finder/
- SWTOR forums, Planet instances player cap raised — https://forums.swtor.com/topic/94654-planet-instances-player-cap-raised
- SWTOR forums, server population cap 3,600 — https://forums.swtor.com/topic/114517-current-final-server-population-cap-3600-players/
- Prima Games, high-population server warning — https://primagames.com/news/swtor-bioware-warns-against-creating-characters-on
- SWTOR blog, Ilum PvP issues — https://www.swtor.com/blog/ilum-pvp-issues
- SWTOR forums, Update on Ilum Open World PvP issues — https://forums.swtor.com/topic/169895-update-on-ilum-open-world-pvp-issues/
- ssilvius/kessel, SWTOR GOM data extractor (fetched directly) — https://github.com/ssilvius/kessel

**SWTOR story and dialogue:**
- PC Gamer, Guinness World Record for voice acting — https://www.pcgamer.com/star-wars-the-old-republic-scoops-guinness-world-record-for-voice-acting/
- GameFront, same record — https://www.gamefront.com/games/gamingtoday/article/swtor-wins-guinness-world-record-for-voice-acting
- SWTOR Wiki (Fandom), Social Points — https://swtor-archive.fandom.com/wiki/Social_Points
- SWTOR Wiki (Fandom), Conversations — https://swtor-archive.fandom.com/wiki/Conversations
- MMORPG.com, "GDC – Localisation in SWTOR" — https://forums.mmorpg.com/discussion/308965/gdc-localisation-in-swtor

**Other MMOs:**
- GitHub search, STO Foundry mission archives — https://github.com/search?q=%22Star+Trek+Online%22+foundry&type=repositories
- NexusForever (WildStar server emulator; fetched) — https://github.com/NexusForever/NexusForever
- CellAO (Anarchy Online server emulator; fetched) — https://github.com/CellAO/CellAO
- The STO Foundry, AO mission, WildStar and ESO megaserver details are mostly **[bk]**. Verify them against
  Cryptic's Foundry announcements (arcgames.com), Funcom's AO pages, and ZeniMax's ESO megaserver FAQ once
  those domains can be reached.
