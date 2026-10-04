-- +goose Up
CREATE TABLE chat.message (id BIGINT PRIMARY KEY, body TEXT NOT NULL);
-- +goose Down
DROP TABLE chat.message;
