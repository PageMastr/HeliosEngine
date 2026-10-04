package platform

type Bus struct{ Listen string }
type Session struct{ Gateways []string }

// The embedded NATS listen port is not a gateway port.
func defaults() (Bus, Session) {
	return Bus{Listen: "127.0.0.1:4222"}, Session{Gateways: []string{"127.0.0.1:7777"}}
}
