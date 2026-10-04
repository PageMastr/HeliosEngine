package orch

import (
	"context"
	"time"

	"github.com/nats-io/nats.go/jetstream"
)

// TTLs assigned to a projection's config and to an ordinary stream after the literal are not lease expiry.
func retain(ctx context.Context, js jetstream.JetStream) {
	presence := jetstream.KeyValueConfig{Bucket: "PRESENCE"}
	presence.TTL = time.Hour
	var events jetstream.StreamConfig
	events.Name = "EVENTS"
	events.MaxAge = 24 * time.Hour
	_, _ = js.CreateKeyValue(ctx, presence)
	_, _ = js.CreateStream(ctx, events)
}

// The same through new(T), a range variable and a collection literal's elided elements.
func retainForms() {
	q := new(jetstream.KeyValueConfig)
	q.Bucket = "PRESENCE"
	q.TTL = time.Hour
	for _, c := range []jetstream.KeyValueConfig{{Bucket: "DIRECTORY"}, {Bucket: "PRESENCE"}} {
		c.TTL = time.Hour
	}
	_ = []jetstream.KeyValueConfig{{Bucket: "PRESENCE", TTL: time.Hour}}
	_ = []jetstream.StreamConfig{{Name: "EVENTS", MaxAge: time.Hour}}
}
