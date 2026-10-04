package platform

// Values of gateway-named types under other names: elided slice and map elements, and an address field.
var pool = []GatewayConfig{{Port: 7040}}
var byRegion = map[string]*GatewayConfig{"eu": {Port: 7041}}
var edge = GatewayEndpoint{Addr: ":7042"}
var gatewayPool = []GatewayConfig{{Port: 7043}}
var gatewayCfg = GatewayConfig{Port: 7044}

type GatewayEndpoint struct{ Addr string }

// Round 7: a nested collection's elided elements and an unexported port field.
var byZone = map[string][]GatewayConfig{"eu": {{Port: 7045}}}
var opts = gatewayOpts{port: 7046}

type gatewayOpts struct{ port int }
