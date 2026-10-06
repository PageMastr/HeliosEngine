-- +goose up
ALTER TABLE svc_identity.profile ADD COLUMN birthday DATE;
-- +goose down
ALTER TABLE svc_identity.profile DROP COLUMN birthday;
