-- +goose Up
CREATE TABLE svc_zeta.base (id BIGINT PRIMARY KEY, label TEXT);
-- +goose Down
DROP TABLE svc_zeta.base;
