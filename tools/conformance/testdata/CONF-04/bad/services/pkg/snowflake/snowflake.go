package snowflake

const (
	nodeBits  = 10
	seqBits   = 12
	timeShift = nodeBits + seqBits
)

// Config keys a node-ID minter would read.
type Config struct {
	WorkerID int64 `toml:"worker_id"`
}

func Next(ms, nodeID, seq int64) int64 {
	return ms<<timeShift | nodeID<<seqBits | seq
}
