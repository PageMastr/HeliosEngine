-- +goose Up
CREATE DOMAIN svc_identity.ip_addr AS inet;
CREATE TABLE svc_identity.visits (id BIGINT PRIMARY KEY, origin svc_identity.ip_addr NOT NULL);
EXPLAIN ANALYZE CREATE TABLE svc_identity.contacts AS SELECT ''::text AS email;
CREATE TABLE svc_identity.seen (origin) AS SELECT '127.0.0.1'::inet;
