package session

import (
	"context"
	"time"

	"github.com/nats-io/nats.go"
)

// Not KV compare-and-set: a store's own four-argument Create in a file that imports only nats.go (the
// legacy KV forms are Create(key, value) and Update(key, value, last)).
type Store interface {
	Create(ctx context.Context, sess string, ttl, ticket time.Duration) (string, error)
}

func open(ctx context.Context, s Store, nc *nats.Conn, sess string, ttl time.Duration) error {
	_ = nc
	_, err := s.Create(ctx, sess, ttl, ttl)
	return err
}
