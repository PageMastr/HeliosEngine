-- +goose Up
CREATE SCHEMA svc_feed_extra CREATE TABLE contact (id BIGINT PRIMARY KEY, email TEXT);
CREATE SCHEMA svc_feed_owned AUTHORIZATION feed_owner CREATE TABLE contact (id BIGINT PRIMARY KEY, email TEXT);
IMPORT FOREIGN SCHEMA remote FROM SERVER legacy_crm INTO svc_feed;
ALTER TABLE svc_feed.log OF svc_feed.log_t;
ALTER TABLE svc_feed.log * NOT OF;
CALL svc_feed.add_contact();
