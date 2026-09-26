-- Identity schema (05 §1.1): accounts, refresh-token families, hash-chained audit log.
-- Expand/contract rule (05 §3.3): later migrations only add; contraction happens two releases on
-- (WP-0.15r's one-time exception is recorded in services/README.md, "Plan conformance").
--
-- Historical: databases created before WP-0.15r applied this file, so its statements stay as they
-- were. They use the schema's old name "identity"; migrations.Up renames it to svc_identity right
-- after this file (05 §1.4, §3), and 00002-00004 replace the plain-text e-mail columns with
-- email_ct and email_bidx under per-account DEKs (05 §6.6).

-- +goose Up
CREATE TABLE identity.account (
    account_id     BIGINT PRIMARY KEY,               -- time-prefixed block ID (05 §1.4.5)
    email          TEXT        NOT NULL,             -- as entered, for display and mail
    email_norm     TEXT        NOT NULL UNIQUE,      -- lowercased, trimmed: the login key
    handle         TEXT        NOT NULL,             -- display name, case preserved
    handle_norm    TEXT        NOT NULL,             -- lowercased handle
    discriminator  SMALLINT    NOT NULL CHECK (discriminator BETWEEN 1 AND 9999),
    password_hash  TEXT        NOT NULL,             -- PHC string: $argon2id$v=19$m=..,t=..,p=..$salt$hash
    is_bot         BOOLEAN     NOT NULL DEFAULT false,
    banned_until   TIMESTAMPTZ,                      -- NULL = not banned; permanent = 9999-12-31
    ban_reason     TEXT,
    created_at     TIMESTAMPTZ NOT NULL,
    updated_at     TIMESTAMPTZ NOT NULL,
    last_login_at  TIMESTAMPTZ,
    CONSTRAINT account_tag_unique UNIQUE (handle_norm, discriminator)
);

-- Refresh tokens are stored hashed (SHA-256 of 32 random bytes). Rotation marks the old row used
-- and inserts the successor in the same family; presenting a used token revokes the family.
CREATE TABLE identity.refresh_token (
    token_hash  BYTEA       PRIMARY KEY,
    family_id   BIGINT      NOT NULL,
    account_id  BIGINT      NOT NULL REFERENCES identity.account (account_id),
    issued_at   TIMESTAMPTZ NOT NULL,
    expires_at  TIMESTAMPTZ NOT NULL,
    used_at     TIMESTAMPTZ,
    revoked_at  TIMESTAMPTZ,
    client_ip   TEXT
);
CREATE INDEX refresh_token_family ON identity.refresh_token (family_id);
CREATE INDEX refresh_token_account ON identity.refresh_token (account_id);

-- Append-only audit log, hash-chained: hash = BLAKE2b-256(prev_hash || canonical row) (05 §1.17).
-- audit_head holds the chain tip; appenders lock it, which serializes the chain.
CREATE TABLE identity.audit_log (
    seq              BIGINT      PRIMARY KEY,
    at               TIMESTAMPTZ NOT NULL,
    actor_account    BIGINT      NOT NULL DEFAULT 0,
    subject_account  BIGINT      NOT NULL DEFAULT 0,
    action           TEXT        NOT NULL,
    client_ip        TEXT        NOT NULL DEFAULT '',
    detail           TEXT        NOT NULL DEFAULT '{}', -- canonical JSON exactly as hashed
    prev_hash        BYTEA       NOT NULL,
    hash             BYTEA       NOT NULL
);
CREATE INDEX audit_log_subject ON identity.audit_log (subject_account, seq);

CREATE TABLE identity.audit_head (
    id    SMALLINT PRIMARY KEY CHECK (id = 1),
    seq   BIGINT   NOT NULL,
    hash  BYTEA    NOT NULL
);
INSERT INTO identity.audit_head (id, seq, hash) VALUES (1, 0, decode(repeat('00', 32), 'hex'));

-- Integrity guard, not business logic: the audit log can only grow, even for the owner role.
-- +goose StatementBegin
CREATE FUNCTION identity.audit_log_append_only() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'identity.audit_log is append-only';
END;
$$;
-- +goose StatementEnd
CREATE TRIGGER audit_log_append_only BEFORE UPDATE OR DELETE OR TRUNCATE ON identity.audit_log
    FOR EACH STATEMENT EXECUTE FUNCTION identity.audit_log_append_only();

-- +goose Down
DROP TRIGGER IF EXISTS audit_log_append_only ON identity.audit_log;
DROP FUNCTION IF EXISTS identity.audit_log_append_only();
DROP TABLE IF EXISTS identity.audit_head;
DROP TABLE IF EXISTS identity.audit_log;
DROP TABLE IF EXISTS identity.refresh_token;
DROP TABLE IF EXISTS identity.account;
