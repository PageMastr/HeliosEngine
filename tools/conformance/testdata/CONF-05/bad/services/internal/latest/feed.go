package latest

// "latest" ends in "test" but is not a test helper.
import "example.com/services/pkg/idgen"

var _ = idgen.Compose
