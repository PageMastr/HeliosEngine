package session

import (
	gen "example.com/services/pkg/idgen"
)

// Session IDs are random 63-bit values; the session service never mints.
func newID(m *gen.Minter) int64 { return m.Next() }
