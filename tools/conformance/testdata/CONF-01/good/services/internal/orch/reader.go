package orch

import (
	"context"

	"github.com/nats-io/nats.go/jetstream"
)

// A reviewed read projection bound through a variable carries a suppression.
func openProjection(ctx context.Context, js jetstream.JetStream, bucket string) (jetstream.KeyValue, error) {
	return js.KeyValue(ctx, bucket) // conformance:allow CONF-01 callers pass BucketDirectory only
}
