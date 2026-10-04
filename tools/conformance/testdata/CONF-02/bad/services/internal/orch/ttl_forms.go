package orch

import (
	"time"

	"github.com/nats-io/nats.go/jetstream"
)

// TTLs on configs reached through new(T), an address, an index, a range variable and a variable set from
// an element, and in elided elements of a collection literal.
func forms(cfgs []jetstream.KeyValueConfig) {
	q := new(jetstream.KeyValueConfig)
	q.Bucket = "LEASES"
	q.TTL = time.Second
	r := jetstream.KeyValueConfig{Bucket: "LEASES"}
	(&r).TTL = time.Second
	cfgs[0].TTL = time.Second
	for _, c := range cfgs {
		c.TTL = time.Second
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
	first := cfgs[1]
	first.TTL = time.Second
	m := map[string]*jetstream.KeyValueConfig{"l": {Bucket: "LEASES"}}
	m["l"].TTL = time.Second
	if v, ok := m["l"]; ok {
		v.TTL = time.Second
	}
}

// A collection from make(…): the file sets no bucket on its elements.
func made(n int) {
	all := make([]jetstream.KeyValueConfig, n)
	all[0].TTL = time.Second
}
