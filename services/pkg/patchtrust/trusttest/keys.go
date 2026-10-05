// Package trusttest derives the test-only Ed25519 keys of the shared trust vectors
// (services/testdata/vectors/trust/). Their seeds are public by construction, so anyone can sign with
// them: they exist only to make the vectors reproducible. Only _test.go files import this package, and
// the verifiers in both languages refuse a root pair that contains one of the test-only roots
// (patchtrust.IsTestOnlyKey, helios::patch::isTestOnlyKey) unless a test sets AllowTestKeys.
package trusttest

import (
	"crypto/ed25519"

	"golang.org/x/crypto/blake2b"
)

// SeedPrefix starts every test-only seed's preimage.
const SeedPrefix = "helios test-only key: "

// Key returns the test-only key called name: its seed is BLAKE2b-256(SeedPrefix + name).
func Key(name string) ed25519.PrivateKey {
	seed := blake2b.Sum256([]byte(SeedPrefix + name))
	return ed25519.NewKeyFromSeed(seed[:])
}

// RootNames are the test-only roots: the vectors' pair (epoch 1 and 2) and a root no pair names.
var RootNames = []string{"root-1", "root-2", "root-x"}
