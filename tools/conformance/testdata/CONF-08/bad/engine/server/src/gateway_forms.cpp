// Address overloads with an octet array, IPv6 groups or bytes, and "host:" + port (or << port).
struct A { net::Address listen = net::Address::ipv4({127, 0, 0, 1}, 7020); };
struct B { net::Address listen = net::Address::ipv4(kLoopbackOctets, 7021); };
struct C { net::Address listen = net::Address::ipv6(kLoopbackGroups, 7022); };
struct D { net::Address listen = net::Address::ipv6Bytes(kLoopbackBytes, 7023); };
inline const std::string kGatewayListenAddr = std::string("0.0.0.0:") + std::to_string(7024);
auto listen = net::Address::parse("127.0.0.1:" + std::to_string(7025));
auto connect = net::Address::parse("127.0.0.1:" + portString());
std::ostringstream listen; void fill() { listen << "0.0.0.0:" << 7026; }
auto listen = std::string{"0.0.0.0:"} + std::to_string(7027);
std::ostringstream listen; void fill2() { listen << "0.0.0.0:" << 7777 + 1; }
net::Address listen =
    net::Address::ipv4(127, 0, 0, 1, 7028);
std::string listen =
    "127.0.0.1:7029";
auto connect = parse(args.get("connect", "",
                              "127.0.0.1:7030"));
