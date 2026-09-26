-- WP-0.15r, contract step (05 §3.3): region_lease is now the only place a holder and a lease
-- generation live (05 §1.4.2), so zone keeps just the zone's identity. As for svc_identity 00004,
-- no shipped release reads the dropped columns (Phase 0), so expand and contract land together.

-- +goose Up
ALTER TABLE svc_orch.zone
    DROP COLUMN owner_process,
    DROP COLUMN lease_gen;

-- +goose Down
ALTER TABLE svc_orch.zone
    ADD COLUMN owner_process BIGINT,
    ADD COLUMN lease_gen     BIGINT NOT NULL DEFAULT 0;
UPDATE svc_orch.zone z SET owner_process = r.holder_proc, lease_gen = r.lease_gen
    FROM svc_orch.region_lease r WHERE r.region_id = z.zone_id;
