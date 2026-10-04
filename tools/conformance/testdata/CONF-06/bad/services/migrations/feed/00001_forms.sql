-- +goose Up
CREATE TABLE svc_feed.log (id BIGINT, body TEXT) PARTITION BY LIST (id);
CREATE TABLE svc_feed.digest AS SELECT 1 AS id WITH NO DATA;
CREATE TABLE public.log_1 PARTITION OF svc_feed.log FOR VALUES IN (1);
CREATE TABLE svc_feed.card OF svc_feed.card_t;
SELECT 1 AS id INTO svc_feed.snapshot;
CREATE MATERIALIZED VIEW svc_feed.top AS SELECT 1 AS id;
CREATE TABLE svc_feed.copy (LIKE svc_feed.nowhere);
CREATE TABLE svc_feed.kid (x INT) INHERITS (svc_feed.nowhere);
ALTER TABLE svc_feed.log RENAME COLUMN missing TO other;
ALTER TABLE svc_feed.log ALTER COLUMN missing TYPE inet;
ALTER TABLE svc_feed.log INHERIT svc_feed.base;
DO $$ BEGIN CREATE TABLE public.hidden (id BIGINT); END $$;
-- +goose Down
DO $$ BEGIN DROP TABLE svc_feed.log; END $$;
