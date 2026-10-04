package orch

import (
	"time"

	"github.com/nats-io/nats.go/jetstream"
)

// A config named like another keeps its own buckets when the file sets them: a parameter whose bucket the
// function assigns, and a named result filled from a projection's literal.
func presence() {
	cfg := jetstream.KeyValueConfig{Bucket: "PRESENCE"}
	_ = cfg
}

func directoryTTL(cfg jetstream.KeyValueConfig) jetstream.KeyValueConfig {
	cfg.Bucket = "DIRECTORY"
	cfg.TTL = time.Hour
	return cfg
}

func projection() (out jetstream.KeyValueConfig) {
	out = jetstream.KeyValueConfig{Bucket: "PRESENCE"}
	out.TTL = time.Hour
	return out
}

func directory() jetstream.KeyValueConfig {
	out := jetstream.KeyValueConfig{Bucket: "DIRECTORY"}
	return out
}
