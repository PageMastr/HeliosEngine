-- Orchestrator schema (05 §1.4, v0): zones, registered processes with epochs, zone lease
-- generations and the placement log. Generations are allocated here before any holder is told.

-- +goose Up
CREATE TABLE orchestrator.zone (
    zone_id        BIGINT      PRIMARY KEY,
    name           TEXT        NOT NULL UNIQUE,
    owner_process  BIGINT,                      -- NULL = unassigned
    lease_gen      BIGINT      NOT NULL DEFAULT 0,
    updated_at     TIMESTAMPTZ NOT NULL
);

CREATE SEQUENCE orchestrator.process_id_seq;

-- One row per registration (incarnation). epoch counts incarnations of the same logical name,
-- so "cell-a" restarted by the supervisor comes back with a higher epoch and fences its past.
CREATE TABLE orchestrator.process (
    process_id     BIGINT      PRIMARY KEY,
    name           TEXT        NOT NULL,
    kind           TEXT        NOT NULL,        -- cell | gateway
    epoch          BIGINT      NOT NULL,
    address        TEXT        NOT NULL DEFAULT '',
    host           TEXT        NOT NULL DEFAULT '',
    pid            INTEGER     NOT NULL DEFAULT 0,
    version        TEXT        NOT NULL DEFAULT '',
    registered_at  TIMESTAMPTZ NOT NULL,
    ended_at       TIMESTAMPTZ,
    end_reason     TEXT,
    CONSTRAINT process_name_epoch UNIQUE (name, epoch)
);
CREATE INDEX process_live ON orchestrator.process (name) WHERE ended_at IS NULL;

CREATE TABLE orchestrator.placement_log (
    seq         BIGSERIAL   PRIMARY KEY,
    at          TIMESTAMPTZ NOT NULL,
    zone_id     BIGINT      NOT NULL,
    process_id  BIGINT,
    lease_gen   BIGINT      NOT NULL,
    action      TEXT        NOT NULL            -- assign | release
);
CREATE INDEX placement_log_zone ON orchestrator.placement_log (zone_id, seq);

-- +goose Down
DROP TABLE IF EXISTS orchestrator.placement_log;
DROP TABLE IF EXISTS orchestrator.process;
DROP SEQUENCE IF EXISTS orchestrator.process_id_seq;
DROP TABLE IF EXISTS orchestrator.zone;
