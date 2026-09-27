package orchestrator

import "testing"

const holderRule = "holder_rule"

func TestConformance(t *testing.T) {
	t.Run(holderRule, testHolderRule)
}

func testHolderRule(t *testing.T) {}
