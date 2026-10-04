-- +goose Up
-- Schemas applies zeta before chat, so svc_zeta.base exists here (name order would put chat first).
CREATE TABLE svc_chat.mirror (LIKE svc_zeta.base INCLUDING ALL);
CREATE TABLE svc_chat.child (extra INT) INHERITS (svc_chat.mirror);
ALTER TABLE svc_chat.mirror ALTER COLUMN label TYPE varchar(100);
ALTER TABLE svc_chat.mirror RENAME COLUMN label TO title;
WITH gone AS (DELETE FROM svc_chat.mirror RETURNING id) INSERT INTO svc_chat.child (id) SELECT id FROM gone;
-- +goose ENVSUB OFF
-- +goose Down
DO $$ BEGIN DROP TABLE svc_chat.child; DROP TABLE svc_chat.mirror; END $$;
