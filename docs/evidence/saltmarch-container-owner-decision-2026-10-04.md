# Saltmarch is an object container in zone `tallis` (repository owner's decision, 2026-10-04)

| | |
|---|---|
| Decision | **Option A.** The Saltmarch outpost is an object container in the single Phase 1 zone `tallis`, not a zone of its own |
| Decided by | The repository owner (the user), on 2026-10-04 |
| Recorded by | The Director (Claude Code agent), on 2026-10-04, as plan revision 13 (09 §5.10.2 D1; `CONSISTENCY.md` §43) |
| Files | `content/zones/tallis/saltmarch.hcont` and `content/zones/tallis/saltmarch.entities/<guid>.hent` (02 §5.5, §5.6) |
| Editing session | `zone-tallis`, the zone session that homes every spatial document under `content/zones/tallis/` (07 §1.8.2) |
| Rework | None. The repository has no `content/` tree yet, and no code places, streams or checks out containers (below) |

## The question and the answer

The Director (the orchestrating agent, 09 §5.1) asked the owner "Which should it be?" and offered two options.
Its task brief for this record summarizes them as follows; the owner's answer below is verbatim.

- **Option A:** Saltmarch is an object container inside the single Phase 1 zone `tallis`, with its files at
  `content/zones/tallis/saltmarch.hcont` and `content/zones/tallis/saltmarch.entities/<guid>.hent`, edited in
  the shared session `zone-tallis`.
- **Option B:** Saltmarch is a zone of its own.

The owner's answer, verbatim:

> Option A

## Why the question came up

At plan revision 12 the plan described Saltmarch two ways.

- **As a container in zone `tallis`:**
  - 02 §5.5: "**Phase 1 layout.** Tallis space and the Harrow surface form one zone, and the station interior
    is a container. BENCH-2 is therefore seamless without a handoff."
  - 02 §5.6: the object-container example is `saltmarch.hcont` plus one `saltmarch.entities/<guid>.hent` per
    entity, with frame parent `body:harrow` and streaming group `"zone.tallis"`.
  - 07 §2.1, T01's Phase 1 MVP: "one zone with its frame hierarchy (Tallis → Harrow → Saltmarch)".
  - 07 §1.8.2: T28's `collab.scope` rule (Error) requires every spatial document to sit under its zone's
    `content/zones/<zone>/` folder, and zone sessions are `zone-<zoneId>`.
- **As a zone:**
  - 07 §1.7.1, the outlier: the sparse-checkout example gave a level designer the cone
    `content/zones/saltmarch/`, a folder that only a zone named `saltmarch` would have.
  - 07 §5.2, ED-10 and ED-20 (Phase 3): "one Saltmarch edit instance", and in ED-20 a session called
    "Saltmarch" beside "a second zone session (Harrow High …)". An edit instance is instance #0 of a zone
    (07 §1.8), so these read Saltmarch as a zone too.

## What changed in the plan (revision 13)

- **02 §5.5.** The Phase 1 layout names its zone, `tallis`, and records this decision: Saltmarch is an object
  container in that zone, with the files and session above.
- **02 §5.6.** A container's source files sit in its zone's folder, here `content/zones/tallis/`. The example
  itself (frame parent `body:harrow`, streaming group `zone.tallis`) already matched and is unchanged.
- **07 §1.7.1.** The level designer's cone is `content/zones/tallis/`. Cone-mode patterns are directories:
  git refuses a file such as `content/zones/tallis/saltmarch.hcont` in cone mode (checked with git 2.43). A
  narrower cone such as `content/zones/tallis/saltmarch.entities/` is legal and brings `saltmarch.hcont` along,
  because cone mode includes the files directly inside each parent folder of a cone. But it leaves out the
  zone's other spatial documents (terrain tiles, `ZonePartitionDef`, other containers' entities). The editor
  opens a zone, not a container (T01's MVP), and the zone session homes the whole folder (07 §1.8.2), so the
  zone folder is the cone.
- **ED-10 and ED-20.** Wording only. The designers and the collab-bots work in Saltmarch through zone
  `tallis`'s edit instance (session `zone-tallis`), and ED-20's cross-session clause runs and publishes beside
  `zone-tallis`. Every threshold and count is unchanged.

The rest of the plan's mentions of Saltmarch name the outpost, its settlement or the "Signal from Saltmarch"
quests, not a zone, and are unchanged. PR #51 lists each of them.

## Code and content

- No `content/` tree exists yet. WP-1.22 (Phase 1 content) creates Saltmarch's files at the paths above.
- The development defaults and tests of `helios-cell` and `helios-gateway` already call the zone `tallis`
  (`--zone tallis`, `--default-zone tallis`; the READMEs of `engine/server`, `apps/cellserver` and
  `apps/gateway`), which matches the decision. Those names are waived IP-lint findings that their owner moves
  to project configuration (`tools/lint/ip_names_policy.cmake`); the decision does not change that either.
- No code implements object containers, the sparse-checkout set-up (`helios-tool new-project` and `doctor`) or
  T28's `collab.scope` yet. Searching the tree outside `docs/` and `third_party/` for `.hcont`, `.hent`,
  `sparse-checkout`, `new-project` and `content/zones` (`git grep`, 2026-10-04) finds only `engine/ecs`'s note
  that a content-placed entity's ID hashes the GUID in its `.hent`, which the decision does not change.
