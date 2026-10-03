package orchestrator

import "testing"

const holderRule = "holder_rule"

func TestConformance(t *testing.T) {
	t.Run(holderRule, testHolderRule)
}

// -short skips it, conditionally; CI runs the Go jobs without -short.
func testHolderRule(t *testing.T) {
	if testing.Short() {
		t.Skip("needs the 60 s outage")
	}
}
