package platform

import (
	"flag"
	"net"
)

const gamePort = 7777

var GatewayPort = gamePort
var port int

func init() { flag.IntVar(&port, "gateway-port", 7777, "the gateway's UDP port") }

func local() Session { return Session{Gateways: []string{net.JoinHostPort("0.0.0.0", "7777")}} }

var GatewayTransport = newTransport()

func newTransport() string { return "udp" }
