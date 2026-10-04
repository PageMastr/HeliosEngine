-- +goose Up
CREATE TABLE svc_identity.tight ("email"TEXT, id BIGINT);
ALTER TABLE svc_identity.tight ADD COLUMN "birthday"DATE;
SELECT ''::text AS "real_name"INTO svc_identity.tight_copy;
