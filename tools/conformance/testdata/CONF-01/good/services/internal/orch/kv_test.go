package orch

import (
	"context"
	"testing"

	"github.com/nats-io/nats.go/jetstream"
)

// Tests that assert the bucket is absent are exempt (09 §5.10.3), in both forms.
func TestNoLeases(t *testing.T) {
	var js jetstream.JetStream
	ctx := context.Background()
	if _, err := js.KeyValue(ctx, "LEASES"); err == nil {
		t.Fatal("there is no LEASES bucket")
	}
	_, err := js.KeyValue(ctx, "orch_leader")
	if err == nil {
		t.Errorf("there is no leader bucket")
	}
}
