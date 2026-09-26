-- WP-0.15r, contract step (05 §3.3): after migration 3 encrypted every address, the plain-text
-- columns go, and with them the last direct PII outside ciphertext and blind indexes (05 §6.6;
-- CONF-07). 05 §3.3 puts a contraction two releases after its expand; Helios has shipped no
-- release that reads these columns (Phase 0, dev databases only), so both steps land in WP-0.15r.

-- +goose Up
ALTER TABLE svc_identity.account
    DROP COLUMN email,        -- the address as entered: now sealed in email_ct
    DROP COLUMN email_norm;   -- the login key and its UNIQUE constraint: now email_bidx

-- +goose Down
-- The addresses cannot be restored from ciphertext here: the columns come back empty.
ALTER TABLE svc_identity.account
    ADD COLUMN email      TEXT,
    ADD COLUMN email_norm TEXT UNIQUE;
