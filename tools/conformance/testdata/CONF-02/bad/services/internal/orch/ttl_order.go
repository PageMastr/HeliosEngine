package orch

import (
	"time"

	"github.com/nats-io/nats.go/jetstream"
)

// A range variable and an element copy over a parameter's collection, before a same-named range and copy
// over a DIRECTORY collection: neither may take the other's bucket, whichever comes first.
func applyAll(cfgs []jetstream.KeyValueConfig) {
	for _, cfg := range cfgs {
		cfg.TTL = time.Second
	}
	cfgs[0].TTL = time.Second
	one := cfgs[1]
	one.TTL = time.Second
}

func directories() {
	dirs := []jetstream.KeyValueConfig{{Bucket: "DIRECTORY"}}
	for _, cfg := range dirs {
		_ = cfg
	}
	one := dirs[0]
	_ = one
}

// The same over a make() collection, which is no parameter: its range variable and its elements keep no
// bucket of a later same-named range.
func applyMade(n int) {
	made := make([]jetstream.KeyValueConfig, n)
	for _, mc := range made {
		mc.TTL = time.Second
	}
	made[0].TTL = time.Second
}

func directoryLoop() {
	for _, mc := range []jetstream.KeyValueConfig{{Bucket: "DIRECTORY"}} {
		_ = mc
	}
}
