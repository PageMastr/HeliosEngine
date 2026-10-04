-- +goose Up
CREATE TABLE svc_identity.login_history (id BIGINT PRIMARY KEY);
ALTER TABLE svc_identity.login_history * ADD COLUMN contact_email TEXT;
