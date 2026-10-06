-- +goose Up
CREATE TABLE chat.room (id BIGINT PRIMARY KEY);             -- the legacy name, after the rename
CREATE SCHEMA IF NOT EXISTS analytics;
CREATE TABLE svc_identity.chat_ban (account_id BIGINT);     -- another service's schema
CREATE TABLE notes (id BIGINT);                             -- whatever the search_path says
ALTER TABLE svc_chat.message SET SCHEMA public;
ALTER SCHEMA svc_chat RENAME TO chat_v2;
-- +goose Down
DROP TABLE chat.room;
