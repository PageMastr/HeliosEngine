package orch

import (
	"time"

	"github.com/nats-io/nats.go/jetstream"
)

// TTLs on configs reached through new(T), an address, an index and a range variable, and in elided
// elements of a collection literal.
func forms(cfgs []jetstream.KeyValueConfig) {
	q := new(jetstream.KeyValueConfig)
	q.Bucket = "LEASES"
	q.TTL = time.Second
	r := jetstream.KeyValueConfig{Bucket: "LEASES"}
	(&r).TTL = time.Second
	cfgs[0].TTL = time.Second
	for _, c := range cfgs {
		c.MaxAge = time.Minute
	}
	leases := []jetstream.KeyValueConfig{{Bucket: "LEASES"}}
	for _, l := range leases {
		l.TTL = time.Second
	}
	for _, s := range []*jetstream.StreamConfig{{Name: "KV_leader"}} {
		s.MaxAge = time.Minute
	}
	_ = []jetstream.KeyValueConfig{{Bucket: "fence", TTL: time.Second}}
	_ = map[string]*jetstream.KeyValueConfig{"a": {Bucket: "leader", LimitMarkerTTL: time.Second}}
}
