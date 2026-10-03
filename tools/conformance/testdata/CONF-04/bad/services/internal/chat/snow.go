package chat

// The same layout by multiplication, in Go.
func snow(ms, node, seq int64) int64 { return ms*(1<<22) + node*(1<<12) + seq }
