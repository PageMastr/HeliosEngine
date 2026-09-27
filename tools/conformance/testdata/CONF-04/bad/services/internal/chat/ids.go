package chat

// A chat message ID composed by hand, outside pkg/idgen.
func messageID(prefix, shard, offset int64) int64 { return prefix<<22 | shard<<17 | offset }
