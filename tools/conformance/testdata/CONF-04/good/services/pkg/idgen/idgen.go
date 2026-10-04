package idgen

const (
	OffsetBits  = 17
	ShardBits   = 5
	shardShift  = OffsetBits
	prefixShift = OffsetBits + ShardBits
)

// Compose is the minter's own composition (05 §1.4.5).
func Compose(prefix int64, shard int, offset uint32) int64 {
	return prefix<<prefixShift | int64(shard)<<shardShift | int64(offset)
}
