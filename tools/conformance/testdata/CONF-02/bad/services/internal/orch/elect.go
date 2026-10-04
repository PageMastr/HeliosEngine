package orch

import (
	"context"
	"fmt"
	"time"

	"github.com/nats-io/nats.go"
	"github.com/nats-io/nats.go/jetstream"
)

// In NATS code, compare-and-set on a key the lint cannot resolve fails closed (both APIs), and so does a
// TTL on a KV bucket it cannot resolve.
func elect(ctx context.Context, js jetstream.JetStream, kv jetstream.KeyValue, lkv nats.KeyValue, key string,
	bucket string, zone int, rev uint64) error {
	_, err := kv.Create(ctx, key, []byte("me"))
	_, _ = kv.Update(ctx, fmt.Sprintf("leader.%d", zone), []byte("me"), rev)
	_, _ = lkv.Create(key, []byte("me"))
	_, _ = lkv.Update(key, []byte("me"), rev)
	_, _ = js.CreateKeyValue(ctx, jetstream.KeyValueConfig{Bucket: bucket, TTL: time.Second})
	return err
}
