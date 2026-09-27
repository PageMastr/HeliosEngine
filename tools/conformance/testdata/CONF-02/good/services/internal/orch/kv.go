package orch

import (
	"context"

	"github.com/nats-io/nats.go/jetstream"
)

func project(ctx context.Context, js jetstream.JetStream, kv jetstream.KeyValue, zone string, rev uint64) error {
	// The directory projection has history but no TTL; its writes are plain puts and zone-keyed CAS.
	_, _ = js.CreateKeyValue(ctx, jetstream.KeyValueConfig{Bucket: "DIRECTORY", History: 5})
	_, _ = kv.Put(ctx, zone, nil)
	_, _ = kv.Update(ctx, "zone.42", nil, rev)
	return nil
}
