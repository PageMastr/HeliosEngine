package orchestrator

import "testing"

// The holder rule's case was renamed, so the required test is gone.
func TestConformance(t *testing.T) {
	t.Run("holder", func(t *testing.T) {})
}

func TestHolderRule(t *testing.T) { t.Run("holder_rule", func(t *testing.T) {}) } // not under TestConformance
