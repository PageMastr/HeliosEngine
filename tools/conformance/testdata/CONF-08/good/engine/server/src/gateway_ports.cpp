constexpr unsigned short kGamePort = 7777;
constexpr unsigned short kGatewayPort = kGamePort;
constexpr const char* kDefaultConnect = "127.0.0.1:7777";
Address listen = Address::ipv4(127, 0, 0, 1, kGamePort);
auto target = parse(args.get("connect", "", kDefaultConnect));
std::printf("helios-gateway %s\n", version::kString);
