package orch

import (
	"time"

	"github.com/nats-io/nats.go/jetstream"
)

// Names are not scoped, so each name below also names a DIRECTORY config. A TTL on a range variable over a
// collection holding a lease bucket, over a parameter's collection, and on a parameter must not borrow it.
func projections() []jetstream.KeyValueConfig {
	lc := jetstream.KeyValueConfig{Bucket: "DIRECTORY"}
	rc := jetstream.KeyValueConfig{Bucket: "DIRECTORY"}
	pc := jetstream.KeyValueConfig{Bucket: "DIRECTORY"}
	return []jetstream.KeyValueConfig{lc, rc, pc}
}

func leaseTTLs() {
	leases := []jetstream.KeyValueConfig{{Bucket: "PRESENCE"}, {Bucket: "LEASES"}}
	for _, lc := range leases {
		lc.TTL = time.Second
	}
}

func eachTTL(cfgs []jetstream.KeyValueConfig) {
	for _, rc := range cfgs {
		rc.TTL = time.Second
	}
}

func oneTTL(pc jetstream.KeyValueConfig) {
	pc.TTL = time.Second
}

// A parameter collection named like a DIRECTORY collection does not take its bucket either, and neither do
// a range variable over it or a copy of one of its elements.
func directorySlice() []jetstream.KeyValueConfig {
	shared := []jetstream.KeyValueConfig{{Bucket: "DIRECTORY"}}
	return shared
}

func sharedTTL(shared []jetstream.KeyValueConfig) {
	shared[0].TTL = time.Second
	for _, sc := range shared {
		sc.TTL = time.Second
	}
	one := shared[1]
	one.TTL = time.Second
}
