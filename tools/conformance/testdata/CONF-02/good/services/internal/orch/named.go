package orch

import (
	"time"

	"github.com/nats-io/nats.go/jetstream"
)

// A parameter or field of another type named like a projection's config leaves that config's bucket alone.
type watcher struct{ opts options }

type options struct{ every time.Duration }

func newWatcher(opts options) watcher { return watcher{opts: opts} }

func watchBucket() {
	opts := jetstream.KeyValueConfig{Bucket: "DIRECTORY"}
	opts.TTL = time.Hour
}
