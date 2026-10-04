package orch

import "github.com/nats-io/nats.go/jetstream"

// A lease bucket in an elided element of a collection literal.
var buckets = []jetstream.KeyValueConfig{{Bucket: "DIRECTORY"}, {Bucket: "LEASES"}}
