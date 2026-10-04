package platform

const defaultGatewayAddr = "0.0.0.0:7000"

type Session struct{ Gateways []string }

func defaults() Session { return Session{Gateways: []string{"127.0.0.1:7777", "127.0.0.1:7778"}} }

var GatewayPort = 7010
