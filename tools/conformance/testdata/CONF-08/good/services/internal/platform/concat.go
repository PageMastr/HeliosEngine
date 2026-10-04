package platform

import (
	"fmt"
	"net"
	"strconv"
)

// 7777 through "host:" + port, fmt.Sprint and a gateway value's Port field.
var gatewayLocal = "127.0.0.1:" + strconv.Itoa(gamePort)
var gatewaySprint = fmt.Sprint(":", 7777)
var gatewayUDP = &net.UDPAddr{Port: gamePort}
var gatewayConfig = GatewayConfig{Port: 7777, AdminPort: 9090}

type GatewayConfig struct{ Port, AdminPort int }

// The same with a host variable: host + ":" + port.
var loopbackHost = "127.0.0.1"
var gatewayHosted = loopbackHost + ":" + strconv.Itoa(gamePort)
