-- +goose Up
CREATE TABLE svc_identity.src (id BIGINT, email TEXT);
CREATE TABLE svc_identity.dst (LIKE svc_identity.src INCLUDING ALL);
ALTER TABLE svc_identity.src DROP COLUMN email;
CREATE TABLE svc_identity.seen (id BIGINT, origin TEXT);
ALTER TABLE svc_identity.seen ALTER COLUMN origin SET DATA TYPE inet;
SELECT ''::text AS email INTO svc_identity.contact;
