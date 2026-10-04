-- +goose Up
CREATE TABLE svc_identity.profile (
    account_id    BIGINT PRIMARY KEY,
    real_name     TEXT,
    date_of_birth DATE,
    seen_from     INET,
    nickname      TEXT,
    legacy_mail   TEXT,
    email_ct      BYTEA
);
