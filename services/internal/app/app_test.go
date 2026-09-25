package app

import (
	"context"
	"errors"
	"strings"
	"testing"
)

type fake struct {
	name     string
	failOn   string
	calls    *[]string
	stopErr  error
	healthOK bool
}

func (f *fake) Name() string { return f.name }
func (f *fake) Start(context.Context) error {
	*f.calls = append(*f.calls, "start:"+f.name)
	if f.failOn == "start" {
		return errors.New("boom")
	}
	return nil
}
func (f *fake) Stop(context.Context) error {
	*f.calls = append(*f.calls, "stop:"+f.name)
	return f.stopErr
}
func (f *fake) Health(context.Context) error {
	if f.healthOK {
		return nil
	}
	return errors.New("unhealthy")
}

func TestStartStopOrder(t *testing.T) {
	var calls []string
	r := NewRunner(nil)
	r.Add(&fake{name: "a", calls: &calls}, &fake{name: "b", calls: &calls}, &fake{name: "c", calls: &calls})
	if err := r.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	if err := r.Stop(context.Background()); err != nil {
		t.Fatal(err)
	}
	if got := strings.Join(calls, ","); got != "start:a,start:b,start:c,stop:c,stop:b,stop:a" {
		t.Fatalf("order %s", got)
	}
	if len(r.Services()) != 3 {
		t.Fatal("services")
	}
	calls = nil
	if err := r.Stop(context.Background()); err != nil || len(calls) != 0 {
		t.Fatal("second stop must be a no-op")
	}
}

func TestStartFailureRollsBack(t *testing.T) {
	var calls []string
	r := NewRunner(nil)
	r.Add(&fake{name: "a", calls: &calls}, &fake{name: "b", calls: &calls, failOn: "start"}, &fake{name: "c", calls: &calls})
	err := r.Start(context.Background())
	if err == nil || !strings.Contains(err.Error(), "start b") {
		t.Fatalf("err %v", err)
	}
	if got := strings.Join(calls, ","); got != "start:a,start:b,stop:a" {
		t.Fatalf("rollback %s", got)
	}
}

func TestStopJoinsErrors(t *testing.T) {
	var calls []string
	r := NewRunner(nil)
	r.Add(&fake{name: "a", calls: &calls, stopErr: errors.New("x")}, &fake{name: "b", calls: &calls, stopErr: errors.New("y")})
	_ = r.Start(context.Background())
	err := r.Stop(context.Background())
	if err == nil || !strings.Contains(err.Error(), "stop a") || !strings.Contains(err.Error(), "stop b") {
		t.Fatalf("joined errors: %v", err)
	}
}
