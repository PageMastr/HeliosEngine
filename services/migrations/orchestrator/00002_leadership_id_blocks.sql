-- Orchestrator leadership anchored in PostgreSQL (05 §1.4.1) and time-prefixed ID blocks
-- (05 §1.4.5). Expand-only: 00001 is left untouched. Historical, like 00001: written against the
-- schema's old name "orchestrator", which migrations.Up renames to svc_orch after this file.

-- +goose Up
-- One row per shard. A replica becomes leader by bumping term once expires_at has passed; every
-- mutating orchestrator transaction re-reads term (FOR SHARE) and aborts when it moved, so a
-- deposed leader's writes touch nothing.
CREATE TABLE orchestrator.orch_leader (
    shard       TEXT        PRIMARY KEY,
    term        BIGINT      NOT NULL,
    holder      TEXT        NOT NULL,
    expires_at  TIMESTAMPTZ NOT NULL
);

-- Block prefixes: last_ms only grows (GREATEST(last_ms + n, now_ms)), so prefixes never repeat
-- whichever replica or process allocates, and no node IDs exist.
CREATE TABLE orchestrator.id_alloc (
    shard    SMALLINT PRIMARY KEY CHECK (shard BETWEEN 0 AND 31),
    last_ms  BIGINT   NOT NULL CHECK (last_ms < 2199023255552)   -- 41 bits
);

-- +goose Down
DROP TABLE IF EXISTS orchestrator.id_alloc;
DROP TABLE IF EXISTS orchestrator.orch_leader;
