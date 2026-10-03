-- +goose Up
/* outer /* inner */ it's still a comment */ ALTER TABLE svc_identity.profile ADD COLUMN first_name TEXT;
