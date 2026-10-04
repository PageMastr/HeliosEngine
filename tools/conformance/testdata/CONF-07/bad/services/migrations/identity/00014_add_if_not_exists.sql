-- +goose Up
CREATE TABLE svc_identity.c (id BIGINT PRIMARY KEY, origin INET);
ALTER TABLE svc_identity.c ADD COLUMN IF NOT EXISTS origin TEXT;
