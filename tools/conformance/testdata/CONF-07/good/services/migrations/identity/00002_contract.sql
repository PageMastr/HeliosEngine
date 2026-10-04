-- The expand/contract pair: the plain-text columns are dropped, so the net schema has none (05 §3.3).
-- +goose Up
ALTER TABLE svc_identity.account DROP COLUMN email, DROP COLUMN IF EXISTS email_norm;
-- +goose Down
ALTER TABLE svc_identity.account ADD COLUMN email TEXT;
