-- +goose Up
INSERT INTO svc_identity.profile (account_id, legacy_mail) VALUES (1, 'first
'); ALTER TABLE svc_identity.profile ADD COLUMN family_name TEXT;
