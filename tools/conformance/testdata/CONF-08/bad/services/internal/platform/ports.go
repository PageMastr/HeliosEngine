package platform

import (
	"fmt"
	"net"
)

const defaultPort = 7014

var FallbackGatewayPort = defaultPort
var EnvGatewayPort = portFromEnv()

func more(host string, port int) Session {
	return Session{Gateways: []string{net.JoinHostPort("127.0.0.1", "7015"), fmt.Sprintf("127.0.0.1:%d", 7016),
		fmt.Sprintf("%s:%d", host, port)}}
}
