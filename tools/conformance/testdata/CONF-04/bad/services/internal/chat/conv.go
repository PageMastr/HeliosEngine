package chat

// The layout by multiplication with converted powers of two.
func conv(ms, shard, seq uint64) uint64 { return ms*uint64(1<<22) + shard*uint64(1<<17) + seq }
