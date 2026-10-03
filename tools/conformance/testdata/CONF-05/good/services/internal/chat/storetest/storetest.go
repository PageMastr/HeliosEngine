package storetest

// A <pkg>test helper outside the minters may use idgen to build fixtures.
import "example.com/services/pkg/idgen"

var _ = idgen.Compose
