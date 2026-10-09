# Harrow High becomes its own zone in Phase 3 (repository owner's decision, 2026-10-06)

| | |
|---|---|
| Decision | **"(A) Container, zone from Ph3 (Recommended)".** In Phases 1 and 2 Harrow High is the station-interior container in zone `tallis`, as 02 §5.5's Phase 1 layout says. From Phase 3 it is a single-cell zone of its own, `harrow-high`, with its own server cell, and `tallis` keeps the orbit and the Saltmarch container |
| Decided by | The repository owner (the user), on 2026-10-06, in answer to the Director's re-asked question of 14:27 UTC (the first ask, at 14:24 UTC, got the answer "explain this with more detail and re-ask me") |
| Recorded by | The Director (Claude Code agent), on 2026-10-09, as part of plan revision 15 (09 §5.10.2 D1; `CONSISTENCY.md` §43; PR #67) |
| Files | Phases 1 and 2: Harrow High's spatial documents sit in zone `tallis`'s folder, `content/zones/tallis/`, as a container (02 §5.6). From Phase 3: in its own zone folder, `content/zones/harrow-high/` |
| Editing session | `zone-tallis` while Harrow High is a container in `tallis`; `zone-harrow-high` once it is a zone (zone sessions are Phase 3, 07 §1.8.2) |
| Rework | None in the tree (below). The cost the owner accepted is Phase 3 work: a content move, and docking and undocking at the station as a zone transition |

## The question and the answer

The Director (the orchestrating agent, 09 §5.1) asked the owner twice on 2026-10-06. Both questions and the
answers are quoted below as the question tool returned them; only the owner's words are the decision.

**First ask (14:24 UTC).** The question:

> Harrow High: 02 §5.5 keeps the station interior a container in zone 'tallis' (Phase 1). ED-20, NS-3.11 and
> NS-4.6 treat Harrow High as its own single-cell zone. Which should the plan say?

The owner's answer, verbatim:

> explain this with more detail and re-ask me

**Re-ask (14:27 UTC).** The question:

> Harrow High is the orbital station over planet Harrow. Its concourse is BENCH-1: 200 avatars, 60 NPCs, 40
> screens. M1 'Descent' starts with undocking a Kestrel from it.
>
> Today the plan describes it two ways:
> • As a container (02 §5.5, Phase 1). Tallis space and Harrow's surface are one zone, `tallis`, run by one
> cell. The station interior is an object container inside it, like Saltmarch. That is what makes the
> orbit-to-interior descent (BENCH-2) seamless: there is no server handoff in Phase 1.
> • As a zone of its own, in three later criteria. ED-20 (Ph3, editor collaboration) runs "a second zone
> session (Harrow High)"; a zone session exists only for a zone. NS-3.11 (Ph3, live server migration)
> migrates "a 500-bot, 20k-entity single-cell hub (Harrow High)" 100 times. NS-4.6 (Ph4, crash recovery)
> kills the cell of "single-cell Harrow High" 10 times under 500 bots.
>
> The question: does the hub get its own zone (and server cell) once the engine can hand players between
> zones (Ph2–3), or does it stay a container in `tallis` forever? Making it a zone from Phase 1 isn't viable:
> zone handoff doesn't exist until Phase 2, so M1's undock would break.

The owner selected **"(A) Container, zone from Ph3 (Recommended)"**. The option's description, as the owner
saw it:

> Phase 1 is unchanged. From Phase 3 the station is its own single-cell zone, so the densest social space
> gets a dedicated server and scales on its own, and the three criteria stand as written. Cost: in Phase 3
> its files move to their own zone folder, and docking or undocking becomes a zone transition (the client
> prefetches the destination; the handoff hitch budget applies).

and its preview:

```
Ph1-2:  zone `tallis` = space + Harrow surface
          ├─ container: Harrow High interior
          └─ container: Saltmarch
Ph3+:   zone `tallis`      (orbit, 4 cells in NS-3.11/4.6)
          └─ container: Saltmarch
        zone `harrow-high` (1 cell, the hub)

ED-20, NS-3.11 (a), NS-4.6 (b): unchanged
02 §5.5: + "Harrow High becomes its own zone in Ph3"
WP impact: a Ph3 content move + dock = zone transition
```

The owner then wrote: "You can now continue with these answers in mind." The other options of the re-ask
are not quoted here, because the Director kept only the chosen option's text. PR #67's description had put
the alternative as: Harrow High stays a container, and ED-20, NS-3.11 and NS-4.6 name another single-cell
hub zone (thresholds unchanged).

## Why the question came up

At plan revision 14 the plan described Harrow High two ways, as the question says.

- **As a container in zone `tallis`:** 02 §5.5's Phase 1 layout ("Tallis space and the Harrow surface form
  one zone, `tallis`, and the station interior is a container. BENCH-2 is therefore seamless without a
  handoff"), and 07 §2.1's T01 MVP, "one zone with its frame hierarchy (Tallis → Harrow → Saltmarch)".
- **As a zone:** ED-20 (07 §5.2: "a second zone session (Harrow High, 6 collab-bots)"), NS-3.11 (a) (04
  §11.4: "a 500-bot, 20k-entity single-cell hub (Harrow High)") and NS-4.6 (b) (04 §11.4: "in single-cell
  Harrow High under 500 bots, 10 `kill -9`s of its cell"). ED-20 and NS-3.11 are Phase 3 criteria and NS-4.6
  is a Phase 4 one.

The decision keeps both texts as written by dating the change: a container in Phases 1 and 2, a zone from
Phase 3.

## What changed in the plan (revision 15)

- **02 §5.5.** The Phase 1 layout bullet is unchanged. A new bullet, "Harrow High becomes its own zone in
  Phase 3", records the decision, the zone ID (`harrow-high`) and its session (`zone-harrow-high`, 07
  §1.8.2), what `tallis` keeps (the 4-cell Harrow orbit of NS-3.11 (c) and NS-4.6 (a), and the Saltmarch
  container), and the two costs below.
- **ED-20, NS-3.11 (a) and NS-4.6 (b).** Unchanged, as the option says: they are Phase 3 and Phase 4 criteria,
  and from Phase 3 Harrow High is the single-cell zone they name.
- **09 §2.** WP-1.22's row (Phase 1 content) says that Harrow High is a container in `tallis` until Phase 3;
  WP-3.9's row (Phase 3 content) makes it the zone `harrow-high`, with its files moved from
  `content/zones/tallis/` and docking as a zone transition (D3: unstarted WPs' rows are edited in the plan
  PR).
- **`CONSISTENCY.md` §43.** A revision 15 row for 02 §5.5 (D1).

## The work-package impact

- **The Phase 3 content move.** WP-3.9 (Phase 3 content; it depends on WP-3.1 to WP-3.7) moves Harrow High's
  documents from `content/zones/tallis/` to `content/zones/harrow-high/`: T28's `collab.scope` rule (07
  §1.8.2) requires every spatial document to sit under its own zone's folder. Like any zone, the new zone
  needs its `ZonePartitionDef` record (02 §5.5; a single-cell zone has one region).
- **Docking and undocking at the station become a zone transition.** 04 §7 lists docking among the zone
  transitions, which are gateway route changes, not reconnects: the client gets `ZoneChange` and "streams
  destination containers during transit VFX (prefetch starts at spool)", and 02 §5.5 says that during a zone
  transition the client "keeps a second view that prefetches the destination". The criteria for a zone
  transition apply to it as written, with their owners in 09 §2:
  - 04 §7's target, "≤ 3 s, 0 reconnects";
  - **NS-1.4** (04 §11.4: "Zone transition ≤ 3 s, 0 reconnects"; WP-1.11, whose scope includes the
    "handoff-seam zone transfer");
  - **CL-4** (08: "Zone transition p99 ≤ 3 s; no frame > 50 ms in world"; WP-1.11 and WP-1.20);
  - **AAA-SRV-5** (01 §3: "Zone transition / handoff p99": ≤ 3 s in Phases 1 and 2, < 100 ms in Phases 3 and
    4, < 50 ms in Phase 5; WP-1.11 for Phase 1, WP-3.1 for Phase 3).

  This record adds no budget and changes none. The option's "the handoff hitch budget applies" (the
  Director's wording) refers to these criteria: from Phase 3 a dock or undock at Harrow High is measured
  against them, AAA-SRV-5's Phase 3 value included, like any other zone transition.

## Code and content

- No code or content places Harrow High yet. The repository's `content/zones/` holds only `tallis`, with the
  Saltmarch container (`saltmarch.hcont` and one `.hent`, each with its `.meta`). `git grep` for "Harrow
  High" outside `docs/` and `third_party/` finds two files, the IP-name policy
  (`tools/lint/ip_names_policy.cmake`) and a lint fixture
  (`tools/lint/tests/ip_names/clean/content/records/ships.jsonc`); neither depends on whether Harrow High is
  a container or a zone.
- The defaults of `helios-cell` and `helios-gateway` name the zone `tallis` (`helios-cell`'s standalone zone
  `tallis:1002`, `helios-gateway --default-zone tallis`), which matches Phases 1 and 2 and does not change.
- No schema, registry, threshold or workflow changes, and no rework WP: the plan's own Phase 3 rows carry the
  work.
