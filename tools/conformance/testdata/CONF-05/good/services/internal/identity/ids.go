package identity

import "example.com/services/pkg/idgen"

func accountID(m *idgen.Minter) int64 { return m.Next() }
