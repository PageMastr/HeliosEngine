package platform

import (
	"errors"
	"flag"
	"fmt"
	"net"
)

const gamePort = 7777

var GatewayPort = gamePort
var port int

func init() { flag.IntVar(&port, "gateway-port", 7777, "the gateway's UDP port") }

func local() Session { return Session{Gateways: []string{net.JoinHostPort("0.0.0.0", "7777")}} }

var GatewayTransport = newTransport()

func newTransport() string { return "udp" }

// A sentence that names the gateway and a port is not an option name: its call carries no port.
func validate(err error) error {
	return fmt.Errorf("gateway address must be ip:port: %w", errors.Join(err, newErr()))
}

func newErr() error { return nil }
