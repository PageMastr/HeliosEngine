constexpr unsigned short kGamePort = 7777;
constexpr unsigned short kGatewayPort = kGamePort;
constexpr const char* kDefaultConnect = "127.0.0.1:7777";
Address listen = Address::ipv4(127, 0, 0, 1, kGamePort);
auto target = parse(args.get("connect", "", kDefaultConnect));
std::printf("helios-gateway %s\n", version::kString);
constexpr const char* kGatewayTransport = "udp";
#define HELIOS_GATEWAY_TRANSPORT udp
auto lport = args.getInt("listen-port", 0, kGamePort);
net::Address listenArr = net::Address::ipv4({127, 0, 0, 1}, 7777);
net::Address listen6 = net::Address::ipv6(kLoopbackGroups, kGamePort);
inline const std::string kGatewayListenAddr = std::string("0.0.0.0:") + std::to_string(kGamePort);
