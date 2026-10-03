package platform

import (
	"fmt"
	"net"
	"strconv"
)

const basePort = 7030

// Addresses built from "host:" and a port, an fmt.Sprint address, and the Port field of a gateway value.
var gatewayAddrs = []string{
	"127.0.0.1:" + strconv.Itoa(7031),
	":" + strconv.Itoa(basePort),
	"127.0.0.1:" + portString(),
}
var gatewaySprint = fmt.Sprint("127.0.0.1:", 7032)
var gatewayListen = &net.UDPAddr{Port: 7033}
var settings = GatewayConfig{Port: 7034}

type GatewayConfig struct{ Port int }

func portString() string { return "7777" }
