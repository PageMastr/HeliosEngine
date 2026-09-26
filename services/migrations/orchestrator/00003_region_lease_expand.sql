-- WP-0.15r, expand step (05 §3.3). From here on the schema is svc_orch: migrations.Up renamed it
-- from "orchestrator" after 00002 (05 §1.4 Owns, §3; CONF-06).
--
-- Region leases (05 §1.4.2, §3.2): one region_lease row per region of a zone instance, and the
-- lease generation lives there, bumped term-fenced before any holder is told. A v0 zone is its own
-- single instance with one region ("Whole"), whose ID is the zone ID, so region_id = instance_id =
-- zone_id and the rows are copied from zone. 00004 drops zone's owner and generation.
--
-- Registration also records the failure domain fd{az, rack, host} and the server build
-- (05 §1.4 API, §1.4.3); Phase 0 stores them, Phase 2's failure detection uses them.

-- +goose Up
CREATE TABLE svc_orch.region_lease (
    region_id    BIGINT      PRIMARY KEY,
    instance_id  BIGINT      NOT NULL,          -- v0: the zone itself (no instances yet)
    holder_proc  BIGINT,                        -- process_id of the holder; NULL = unassigned
    lease_gen    BIGINT      NOT NULL DEFAULT 0 CHECK (lease_gen >= 0),
    assigned_at  TIMESTAMPTZ                    -- when holder_proc was assigned
);
INSERT INTO svc_orch.region_lease (region_id, instance_id, holder_proc, lease_gen, assigned_at)
    SELECT zone_id, zone_id, owner_process, lease_gen,
           CASE WHEN owner_process IS NULL THEN NULL ELSE updated_at END
    FROM svc_orch.zone;

ALTER TABLE svc_orch.process
    ADD COLUMN az           TEXT   NOT NULL DEFAULT '',   -- fd.az (host is the existing column)
    ADD COLUMN rack         TEXT   NOT NULL DEFAULT '',   -- fd.rack
    ADD COLUMN server_build BIGINT NOT NULL DEFAULT 0 CHECK (server_build >= 0);

ALTER TABLE svc_orch.placement_log ADD COLUMN region_id BIGINT;
UPDATE svc_orch.placement_log SET region_id = zone_id;

-- +goose Down
-- Below this point lies the rename of schema orchestrator, which goose cannot undo: restore a
-- backup. (00004's down is reversible and restores zone's owner and generation.)
-- +goose StatementBegin
DO $$ BEGIN RAISE EXCEPTION 'svc_orch 00003 is irreversible (it follows the schema rename): restore a backup'; END $$;
-- +goose StatementEnd
