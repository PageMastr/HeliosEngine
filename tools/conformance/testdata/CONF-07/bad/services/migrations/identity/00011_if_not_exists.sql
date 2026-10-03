-- +goose Up
CREATE TABLE svc_identity.reach (id BIGINT PRIMARY KEY, email TEXT);
CREATE TABLE IF NOT EXISTS svc_identity.reach (id BIGINT PRIMARY KEY);
