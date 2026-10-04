package orch

import (
	"context"

	"github.com/nats-io/nats.go/jetstream"

	"example.com/services/internal/names"
)

const leaderBucket = "orch_leader"

func open(ctx context.Context, js jetstream.JetStream) error {
	if _, err := js.CreateKeyValue(ctx, jetstream.KeyValueConfig{Bucket: "LEASES"}); err != nil {
		return err
	}
	cfg := jetstream.KeyValueConfig{Bucket: leaderBucket, History: 1}
	if _, err := js.CreateOrUpdateKeyValue(ctx, cfg); err != nil {
		return err
	}
	_, err := js.KeyValue(ctx, names.LeaseBucket)
	return err
}
