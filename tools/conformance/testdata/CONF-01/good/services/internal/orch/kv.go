package orch

import (
	"context"

	"github.com/nats-io/nats.go/jetstream"
)

// BucketDirectory is the read projection of region_lease (zone -> owning cell).
const BucketDirectory = "DIRECTORY"

func open(ctx context.Context, js jetstream.JetStream) error {
	// No LEASES bucket: leases live in PostgreSQL. (A comment may say so.)
	_, err := js.CreateOrUpdateKeyValue(ctx, jetstream.KeyValueConfig{Bucket: BucketDirectory, History: 5})
	return err
}
