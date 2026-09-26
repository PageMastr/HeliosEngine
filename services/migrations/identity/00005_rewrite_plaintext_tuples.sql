-- +goose NO TRANSACTION
-- WP-0.15r: rewrite the tables that held plain-text PII. Migration 3 wrote new row versions with
-- the plain-text columns nulled and 00004 dropped the columns, but the old row versions stay in
-- the heap until vacuumed, and a dropped column's values stay in live rows until a rewrite.
-- VACUUM FULL copies every live row with dropped columns set to NULL and discards the rest.
-- It cannot run in a transaction, hence NO TRANSACTION. On a fresh database it is instant.

-- +goose Up
VACUUM FULL svc_identity.account;
VACUUM FULL svc_identity.refresh_token;
VACUUM FULL svc_identity.audit_log;

-- +goose Down
-- +goose StatementBegin
DO $$ BEGIN RAISE EXCEPTION 'svc_identity 00005 is irreversible: restore a backup'; END $$;
-- +goose StatementEnd
