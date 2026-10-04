package orch

import (
	"context"

	"github.com/nats-io/nats.go"
	"github.com/nats-io/nats.go/jetstream"
)

// In NATS code, a bucket the lint cannot resolve fails closed: a parameter, a config variable whose
// bucket this file never sets, a config literal or a Bucket assignment from a parameter, and the same
// in the legacy API. A config whose bucket this file sets is checked where it is set (lines 19 and 21),
// not again at the call.
func bind(ctx context.Context, js jetstream.JetStream, ljs nats.JetStreamContext, name string,
	cfg jetstream.KeyValueConfig) {
	_, _ = js.KeyValue(ctx, name)
	_, _ = js.CreateKeyValue(ctx, cfg)
	var other jetstream.KeyValueConfig
	other.Bucket = name
	_, _ = js.UpdateKeyValue(ctx, other)
	named := jetstream.KeyValueConfig{Bucket: name, History: 1}
	_, _ = js.CreateOrUpdateKeyValue(ctx, named)
	_, _ = ljs.KeyValue(name)
	_, _ = ljs.CreateKeyValue(&nats.KeyValueConfig{Bucket: name})
}
