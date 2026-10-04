package orch

import "github.com/nats-io/nats.go"

// nats.go's legacy KV API: Create(key, value) and Update(key, value, last) are compare-and-set too.
func elect(kv nats.KeyValue, rev uint64) {
	_, _ = kv.Create("orch.leader", []byte("me"))
	_, _ = kv.Update("orch.leader", []byte("me"), rev)
	_, _ = kv.Create("zone.7", []byte("x"))
}
