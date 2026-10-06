package batch

// A converted or scaled literal is a size, not a time-prefix field.
const maxBytes = uint64(1) << 22
const total = 4 * (1 << 22)
