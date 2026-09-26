-- WP-0.15r, contract step (05 §3.3): after migration 3 encrypted every stored address and client
-- IP and nulled the plain-text copies, the plain-text columns go (05 §6.6; CONF-07). 00005 then
-- rewrites the tables so no dead tuple keeps them. Plain text written before the upgrade can
-- still be in WAL and in base backups taken before it.
--
-- Deviation from 05 §3.3, which puts a contraction two releases after its expand: Helios has
-- shipped no release that reads these columns (Phase 0, dev databases only), so both steps land
-- in WP-0.15r (services/README.md, "Plan conformance").

-- +goose Up
ALTER TABLE svc_identity.account
    DROP COLUMN email,        -- the address as entered: now sealed in email_ct
    DROP COLUMN email_norm;   -- the login key and its UNIQUE constraint: now email_bidx
ALTER TABLE svc_identity.refresh_token DROP COLUMN client_ip;    -- now client_ip_ct
ALTER TABLE svc_identity.audit_log DROP COLUMN client_ip;        -- now login_history (not chained)

-- +goose Down
-- +goose StatementBegin
DO $$ BEGIN RAISE EXCEPTION 'svc_identity 00004 is irreversible: the plain text is gone; restore a backup'; END $$;
-- +goose StatementEnd
