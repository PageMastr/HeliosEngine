package orch

import (
	"context"
	"testing"

	"github.com/nats-io/nats.go/jetstream"
)

// A test that binds a fence bucket and uses it is not an absence check.
func TestFence(t *testing.T) {
	var js jetstream.JetStream
	kv, err := js.KeyValue(context.Background(), "fences")
	if err != nil {
		t.Fatal(err)
	}
	_ = kv
}
