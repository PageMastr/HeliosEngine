struct Cfg { Address listen = Address::ipv4(0, 0, 0, 0, 7005); };
auto target = parse(args.get("connect", "", "127.0.0.1:7006"));
constexpr unsigned short kListenPort = 7012;
constexpr const char* kDefaultConnect = "127.0.0.1:7013";
Address listen = Address::ipv4(0, 0, 0, 0,
                               kListenPort);
auto probe = parse(args.get("connect", "", kDefaultConnect));
auto bound = parse(args.get("listen", "", defaultListen()));
auto lport = args.getInt("listen-port", 0, kListenPort);
