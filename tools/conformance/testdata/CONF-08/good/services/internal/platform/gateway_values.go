package platform

// The same at 7777; a gateway value under a gateway name is read once.
var pool = []GatewayConfig{{Port: 7777, AdminPort: 9090}}
var edge = GatewayEndpoint{Addr: ":7777"}
var gatewayPool = []GatewayConfig{{Port: gamePort}}

type GatewayEndpoint struct{ Addr string }
