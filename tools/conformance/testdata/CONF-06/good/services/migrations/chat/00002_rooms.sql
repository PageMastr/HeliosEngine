-- Comments may say CREATE TABLE public.x; and strings may too.
-- +goose Up
-- +goose StatementBegin
CREATE FUNCTION svc_chat.touch() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    CREATE TABLE public.never_run (id BIGINT); -- a function body is not a migration statement
    RETURN NEW;
END;
$$;
-- +goose StatementEnd
CREATE TABLE svc_chat.room (id BIGINT PRIMARY KEY, note TEXT DEFAULT 'CREATE SCHEMA x;');
/* CREATE SCHEMA hidden; */
-- +goose Down
CREATE SCHEMA undo_only;
DROP TABLE svc_chat.room;
