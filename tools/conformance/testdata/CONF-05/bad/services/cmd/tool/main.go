package main

import "context"

type registry interface {
	AllocateIdBlocks(ctx context.Context, id, epoch int64, n int) ([]int64, error)
}

func main() {
	var r registry
	_, _ = r.AllocateIdBlocks(context.Background(), 1, 1, 2)
}
