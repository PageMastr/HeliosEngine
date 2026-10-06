-- +goose Up
INSERT INTO svc_identity.profile (account_id, legacy_mail) VALUES (0, E'it\'s');
ALTER TABLE svc_identity.profile ADD COLUMN given_name TEXT;
