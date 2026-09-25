package orchestrator_test

import (
	"testing"

	"github.com/PageMastr/scifi-test/services/internal/orchestrator"
	"github.com/PageMastr/scifi-test/services/internal/orchestrator/storetest"
)

func TestMemStoreConformance(t *testing.T) {
	storetest.Run(t, func(*testing.T) orchestrator.Store { return orchestrator.NewMemStore() })
}
