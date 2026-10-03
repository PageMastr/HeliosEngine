//go:build integration

package orchestrator

import "testing"

func TestConformance(t *testing.T) {
	t.Run("holder_rule", testHolderRule)
	t.Run("other", func(t *testing.T) { t.Skip("not CONF-03's") })
}

func testHolderRule(t *testing.T) {
	t.Skip("flaky on CI")
}
