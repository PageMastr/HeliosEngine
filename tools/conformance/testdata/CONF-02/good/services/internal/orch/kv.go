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

// A reviewed compare-and-set on a key the lint cannot resolve carries a suppression.
func claimZone(ctx context.Context, kv jetstream.KeyValue, zone string, rev uint64) error {
	_, err := kv.Update(ctx, zone, nil, rev) // conformance:allow CONF-02 DIRECTORY zone keys, not leader state
	return err
}
