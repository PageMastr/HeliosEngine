package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/PageMastr/scifi-test/services/pkg/keyring"
)

func noenv(string) string { return "" }

func TestCommands(t *testing.T) {
	var out bytes.Buffer
	if code := run([]string{"version"}, noenv, &out); code != 0 || !strings.Contains(out.String(), "helios-backend dev") {
		t.Fatalf("version: %d %q", code, out.String())
	}
	out.Reset()
	if code := run([]string{"help"}, noenv, &out); code != 0 || !strings.Contains(out.String(), "keys rotate") {
		t.Fatalf("help: %d", code)
	}
	if code := run([]string{"frobnicate"}, noenv, &out); code != 2 {
		t.Fatalf("unknown command: %d", code)
	}
	if code := run([]string{"run", "--no-such-flag"}, noenv, &out); code != 2 {
		t.Fatalf("bad flag: %d", code)
	}
	if code := run([]string{"run", "--shard-index", "99"}, noenv, &out); code != 2 {
		t.Fatalf("invalid config: %d", code)
	}
	if code := run([]string{"run", "-h"}, noenv, &out); code != 0 || !strings.Contains(out.String(), "HELIOS_DB") {
		t.Fatalf("flag help: %d", code)
	}
	if code := run([]string{"run", "stray"}, noenv, &out); code != 2 {
		t.Fatalf("stray argument: %d", code)
	}
}

func TestKeysRotate(t *testing.T) {
	dir := t.TempDir()
	var out bytes.Buffer
	for i := 0; i < 4; i++ {
		if code := run([]string{"keys", "rotate", "--data", dir}, noenv, &out); code != 0 {
			t.Fatalf("rotate %d: %d %s", i, code, out.String())
		}
	}
	ring, err := keyring.Load(filepath.Join(dir, "keys", "netcode-shard.json"))
	if err != nil {
		t.Fatal(err)
	}
	// Created with id 1, rotated four times, pruned to three generations.
	if ring.Current().ID != 5 || len(ring.Keys) != 3 {
		t.Fatalf("ring: current %d, %d keys", ring.Current().ID, len(ring.Keys))
	}
	if code := run([]string{"keys", "rotate", "jwt", "--data", dir}, noenv, &out); code != 0 {
		t.Fatal("jwt rotate")
	}
	if code := run([]string{"keys", "rotate", "nats", "--data", dir}, noenv, &out); code != 0 {
		t.Fatal("nats fleet password rotate")
	}
	if _, err := keyring.Load(filepath.Join(dir, "keys", "nats-fleet.json")); err != nil {
		t.Fatal(err)
	}
	if code := run([]string{"keys", "rotate", "nope", "--data", dir}, noenv, &out); code != 2 {
		t.Fatal("unknown key set")
	}
	if code := run([]string{"keys", "--data", dir}, noenv, &out); code != 2 {
		t.Fatal("missing subcommand")
	}
}

func TestReset(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "data")
	if err := os.MkdirAll(filepath.Join(dir, "keys"), 0o700); err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	if code := run([]string{"reset", "--data", dir}, noenv, &out); code != 2 {
		t.Fatal("reset without --yes must refuse")
	}
	if _, err := os.Stat(dir); err != nil {
		t.Fatal("data removed without confirmation")
	}
	if code := run([]string{"reset", "--yes", "--data", dir}, noenv, &out); code != 0 {
		t.Fatalf("reset: %s", out.String())
	}
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Fatal("data dir still there")
	}
}

func TestRedact(t *testing.T) {
	if got := redact("postgres://helios:secret@db:5432/helios"); strings.Contains(got, "secret") {
		t.Fatalf("password leaked: %s", got)
	}
	if got := redact("embedded"); got != "embedded" {
		t.Fatal(got)
	}
	if got := redact("nats://fleet:pw123@10.0.0.1:4222"); strings.Contains(got, "pw123") {
		t.Fatalf("bus password leaked: %s", got)
	}
}
