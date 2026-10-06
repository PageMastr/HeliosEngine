-- +goose Up
ALTER TABLE svc_identity.profile RENAME COLUMN nickname TO email;
ALTER TABLE svc_identity.profile ADD COLUMN client_ip TEXT, ADD COLUMN phone_hash BYTEA;
