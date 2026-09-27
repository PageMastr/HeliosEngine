-- +goose Up
CREATE TABLE svc_identity.account (account_id BIGINT PRIMARY KEY, email TEXT, email_norm TEXT, address TEXT);
ALTER TABLE svc_identity.account ADD COLUMN email_ct BYTEA, ADD COLUMN email_bidx BYTEA;
CREATE TABLE svc_identity.login_event (at TIMESTAMPTZ, kind TEXT) PARTITION BY RANGE (at);
