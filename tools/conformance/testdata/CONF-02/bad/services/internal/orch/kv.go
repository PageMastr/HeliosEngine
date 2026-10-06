package orch

import (
	"context"
	"time"

	"github.com/nats-io/nats.go/jetstream"
)

const leaderKey = "orch.leader"

func run(ctx context.Context, js jetstream.JetStream, kv jetstream.KeyValue, key string, rev uint64) error {
	_, _ = js.CreateKeyValue(ctx, jetstream.KeyValueConfig{
		Bucket: "LEASES",
		TTL:    3 * time.Second,
	})
	_, _ = js.CreateStream(ctx, jetstream.StreamConfig{Name: "KV_fence", MaxAge: time.Minute})
	_, _ = kv.Create(ctx, key, nil, jetstream.KeyTTL(3*time.Second))
	_, _ = kv.Create(ctx, "region.lease.7", nil, jetstream.KeyTTL(time.Second))
	_, _ = kv.Update(ctx, leaderKey, []byte("me"), rev)
	return nil
}
