package orch

import (
	"context"

	"github.com/nats-io/nats.go/jetstream"
)

func assigned(ctx context.Context, js jetstream.JetStream) error {
	var cfg jetstream.KeyValueConfig
	cfg.Bucket = "region_leases"
	_, err := js.CreateKeyValue(ctx, cfg)
	return err
}

const leaseSubject = "$KV.LEASES.zone.7"
