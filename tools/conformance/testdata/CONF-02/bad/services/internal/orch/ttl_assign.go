package orch

import (
	"context"
	"time"

	"github.com/nats-io/nats.go/jetstream"
)

// TTLs assigned after the literal: on a lease bucket, on a config whose bucket the lint cannot resolve (a
// parameter), on a field of that type, and on a lease bucket's KV_ stream.
type setup struct{ kv jetstream.KeyValueConfig }

func assign(ctx context.Context, js jetstream.JetStream, cfg *jetstream.KeyValueConfig, s *setup) {
	lease := jetstream.KeyValueConfig{Bucket: "LEASES"}
	lease.TTL = 3 * time.Second
	cfg.LimitMarkerTTL = time.Second
	s.kv.MaxAge = time.Minute
	sc := jetstream.StreamConfig{Name: "KV_fence"}
	sc.MaxAge = time.Minute
	_, _ = js.CreateKeyValue(ctx, lease)
	_, _ = js.CreateStream(ctx, sc)
}
