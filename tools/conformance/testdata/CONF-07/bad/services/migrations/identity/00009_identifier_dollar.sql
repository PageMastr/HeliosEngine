-- +goose Up
ALTER TABLE svc_identity.profile ADD COLUMN tag$v$ TEXT, ADD COLUMN legal_name TEXT;
