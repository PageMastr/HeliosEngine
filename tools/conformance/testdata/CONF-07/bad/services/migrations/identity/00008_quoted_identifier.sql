-- +goose Up
ALTER TABLE svc_identity.profile ADD COLUMN "it's" TEXT, ADD COLUMN last_name TEXT;
