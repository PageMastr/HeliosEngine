package rpc

import "time"

// SetNATSConfirmTimeout shortens NATSHandle's confirmation round trip for a test and returns a
// function that restores it. Not safe while NATSHandle runs on another goroutine.
func SetNATSConfirmTimeout(d time.Duration) (restore func()) {
	prev := natsConfirmTimeout
	natsConfirmTimeout = d
	return func() { natsConfirmTimeout = prev }
}

// RefusedSubscription exposes refusedSubscription to the external test package.
var RefusedSubscription = refusedSubscription
