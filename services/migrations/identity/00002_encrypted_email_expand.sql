-- WP-0.15r, expand step (05 §3.3). From here on the schema is svc_identity: migrations.Up renamed
-- it from "identity" after 00001 (05 §1.4, §3; CONF-06).
--
-- Direct PII is encrypted under a per-account data key (05 §3.2, §6.6 Phase 0 rule): subject_key
-- holds each account's 256-bit DEK wrapped by the KEK, email_ct the address sealed with
-- XChaCha20-Poly1305 (AAD = table, column, account_id), and email_bidx HMAC-SHA256(pepper,
-- lower(email)), the login key. Migration 3 (Go, migrations.go) encrypts existing rows; 00004
-- drops the plain-text columns.

-- +goose Up
CREATE TABLE svc_identity.subject_key (
    account_id    BIGINT      PRIMARY KEY REFERENCES svc_identity.account (account_id),
    wrapped_dek   BYTEA,                        -- NULL once shredded: the account's PII is then unreadable
    kek_version   INT         NOT NULL,         -- KEK generation that wrapped it (keyring id)
    analytics_id  BIGINT      UNIQUE,           -- pseudonymous telemetry ID (05 §6.6); not minted yet
    shredded_at   TIMESTAMPTZ,                  -- erasure (Phase 3)
    CONSTRAINT subject_key_shredded CHECK ((wrapped_dek IS NULL) = (shredded_at IS NOT NULL))
);

-- Nullable: erasure (Phase 3) clears them, as in the 05 §3.2 sketch.
ALTER TABLE svc_identity.account
    ADD COLUMN email_ct   BYTEA,
    ADD COLUMN email_bidx BYTEA,
    ADD CONSTRAINT account_email_bidx_unique UNIQUE (email_bidx);

-- The append-only guard moved with the schema; only its message named the old one.
-- +goose StatementBegin
CREATE OR REPLACE FUNCTION svc_identity.audit_log_append_only() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'svc_identity.audit_log is append-only';
END;
$$;
-- +goose StatementEnd

-- +goose Down
-- The guard keeps its corrected message: the schema stays svc_identity either way.
ALTER TABLE svc_identity.account
    DROP CONSTRAINT IF EXISTS account_email_bidx_unique,
    DROP COLUMN IF EXISTS email_bidx,
    DROP COLUMN IF EXISTS email_ct;
DROP TABLE IF EXISTS svc_identity.subject_key;
