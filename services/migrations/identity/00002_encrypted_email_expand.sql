-- WP-0.15r, expand step (05 §3.3). From here on the schema is svc_identity: migrations.Up renamed
-- it from "identity" after 00001 (05 §1.4, §3; CONF-06).
--
-- Direct PII is encrypted under a per-account data key (05 §3.2, §6.6 Phase 0 rule; CONF-07):
-- - subject_key holds each account's 256-bit DEK wrapped by the KEK;
-- - email_ct is the address sealed with XChaCha20-Poly1305 (AAD = table, column, account_id) and
--   email_bidx = HMAC-SHA256(pepper, lower(email)) is the login key;
-- - client IPs are sealed the same way: refresh_token.client_ip_ct (bound to the token row), and
--   login_history, which takes the IPs out of the chained audit rows (05 §1.17: rows hold only
--   pseudonymous IDs) and keeps them 90 days (05 §6.6). Unknown logins have no subject: no IP.
-- - audit_log.note_digest is 05 §1.17's BLAKE2b digest of a sealed audit_note (none yet).
-- Migration 3 (Go, migrations.go) encrypts what the old schema stored in plain text, nulls the
-- plain-text copies and re-chains the audit log without IPs; 00004 drops the plain-text columns
-- and 00005 rewrites the tables so no old tuple keeps them.

-- +goose Up
CREATE TABLE svc_identity.subject_key (
    account_id    BIGINT      PRIMARY KEY REFERENCES svc_identity.account (account_id),
    wrapped_dek   BYTEA,                        -- NULL once shredded: the account's PII is then unreadable
    kek_version   INT         NOT NULL,         -- KEK generation that wrapped it (keyring id)
    analytics_id  BIGINT      UNIQUE,           -- pseudonymous telemetry ID (05 §6.6); not minted yet
    shredded_at   TIMESTAMPTZ,                  -- erasure (Phase 3)
    CONSTRAINT subject_key_shredded CHECK ((wrapped_dek IS NULL) = (shredded_at IS NOT NULL))
);

-- Nullable: erasure (Phase 3) clears them, as in the 05 §3.2 sketch. The plain-text columns lose
-- NOT NULL so migration 3 can null them before 00004 drops them.
ALTER TABLE svc_identity.account
    ADD COLUMN email_ct   BYTEA,
    ADD COLUMN email_bidx BYTEA,
    ADD CONSTRAINT account_email_bidx_unique UNIQUE (email_bidx),
    ALTER COLUMN email DROP NOT NULL,
    ALTER COLUMN email_norm DROP NOT NULL;

ALTER TABLE svc_identity.refresh_token ADD COLUMN client_ip_ct BYTEA;

ALTER TABLE svc_identity.audit_log
    ADD COLUMN note_digest BYTEA,
    ALTER COLUMN client_ip DROP NOT NULL,
    ALTER COLUMN client_ip DROP DEFAULT;

-- One row per IP seen for an account. No foreign key: retention and erasure drop these rows on
-- their own schedule (05 §3.3 plans daily partitions), independent of the account row.
CREATE TABLE svc_identity.login_history (
    event_id      BIGINT      PRIMARY KEY,      -- block ID (migrated rows: -audit seq); in the IP's AAD
    account_id    BIGINT      NOT NULL,
    action        TEXT        NOT NULL,         -- the audit action it accompanies
    at            TIMESTAMPTZ NOT NULL,
    client_ip_ct  BYTEA       NOT NULL          -- sealed under the account's DEK (AAD = table, column, event_id)
);
CREATE INDEX login_history_at ON svc_identity.login_history (at);                      -- PurgeLoginHistory
CREATE INDEX login_history_account ON svc_identity.login_history (account_id, at);     -- LoginHistory

-- The append-only guard moved with the schema; only its message named the old one.
-- +goose StatementBegin
CREATE OR REPLACE FUNCTION svc_identity.audit_log_append_only() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'svc_identity.audit_log is append-only';
END;
$$;
-- +goose StatementEnd

-- +goose Down
-- Below this point lies the rename of schema identity, which goose cannot undo: restore a backup.
-- +goose StatementBegin
DO $$ BEGIN RAISE EXCEPTION 'svc_identity 00002 is irreversible (it follows the schema rename): restore a backup'; END $$;
-- +goose StatementEnd
