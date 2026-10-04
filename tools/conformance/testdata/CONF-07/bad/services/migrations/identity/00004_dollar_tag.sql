-- +goose Up
ALTER TABLE svc_identity.profile ADD COLUMN surname TEXT;
-- +goose StatementBegin
CREATE FUNCTION svc_identity.noop() RETURNS void AS $fn1$
BEGIN
    PERFORM 1; ALTER TABLE svc_identity.profile DROP COLUMN surname;
END
$fn1$ LANGUAGE plpgsql;
-- +goose StatementEnd
