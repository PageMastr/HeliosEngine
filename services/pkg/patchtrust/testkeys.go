package patchtrust

import "encoding/hex"

// testOnlyRoots are the public keys of the shared vectors' test-only roots (package trusttest:
// root-1, root-2, root-x, whose seeds are BLAKE2b-256("helios test-only key: <name>"), public by
// construction). engine/patch's isTestOnlyKey lists the same keys; TestTestOnlyRoots checks both lists
// against the derivation.
var testOnlyRoots = [...]string{
	"157c930ac055ce5cbc6dc770910d6f6a1a816776e3e5c6460cdea0002bb76ccf",
	"724fc80638768e34218d5f9877627890b4caa0a6ab2a7894f573f6ede14bae7a",
	"7171f6eba59c768db892730795613f1abe75b89052a05fcc6e315b05689d7e7e",
}

// IsTestOnlyKey reports whether pub is one of the test-only roots. NewVerifier refuses a root pair that
// contains one unless Options.AllowTestKeys is set, which only tests do.
func IsTestOnlyKey(pub PublicKey) bool {
	for _, h := range testOnlyRoots {
		if hex.EncodeToString(pub[:]) == h {
			return true
		}
	}
	return false
}
