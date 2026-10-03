package cache

import "context"

// Not NATS code: a same-named method of another type, with a bucket from a variable, is not read as a
// JetStream KV bind.
type Store struct{}

func (Store) KeyValue(ctx context.Context, bucket string) error { return nil }

func use(ctx context.Context, s Store, bucket string) error { return s.KeyValue(ctx, bucket) }
