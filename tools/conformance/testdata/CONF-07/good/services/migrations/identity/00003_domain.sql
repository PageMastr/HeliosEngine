-- A domain over text is not an address type, and EXPLAIN without ANALYZE runs nothing.
-- +goose Up
CREATE DOMAIN svc_identity.handle AS text CHECK (VALUE <> '');
CREATE TABLE svc_identity.handles (id BIGINT PRIMARY KEY, handle svc_identity.handle);
EXPLAIN CREATE TABLE svc_identity.contacts AS SELECT ''::text AS email;
