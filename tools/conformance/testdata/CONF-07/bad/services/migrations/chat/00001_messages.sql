-- +goose Up
CREATE TABLE svc_chat.message (id BIGINT PRIMARY KEY, sender_email_ct BYTEA, body TEXT);
