// The gateway listens on 7777; its trunk to the cells uses other ports, which are not the game port.
auto listen = parse(args.get("listen", "", "127.0.0.1:7777"));
auto cell = parse(args.get("cell", "", "127.0.0.1:7810"));
// A block's body starts a new statement (after `) const {` and `) {`); an initializer's braces do not.
net::Address connect() const {
    return net::Address::ipv4(127, 0, 0, 1, 7813);
}
void connect(bool remote) {
    auto trunk = net::Address::ipv4(127, 0, 0, 1, 7814);
}
struct GatewayOpts {
    net::Address listen{net::Address::ipv4(127, 0, 0, 1, 7777)};
    net::Address trunk{
        net::Address::ipv4(127, 0, 0, 1, 7815)};
};
namespace gateway {
auto trunk = net::Address::ipv4(127, 0, 0, 1, 7816);
}
void probe() {
#if HELIOS_FAST
    start(fast,
#else
    start(slow,
#endif
          1);
}
auto listen = net::Address::ipv4(127, 0, 0, 1, 7777);
auto trunk =
    net::Address::ipv4(127, 0, 0, 1, 7817);
// Names whose words are not listen or connect: a trunk socket's bind address, a connected-peers scrape.
net::Address trunkBind = net::Address::anyV4(7818);
auto connectedPeers = scrape("10.0.0.9:9100");
