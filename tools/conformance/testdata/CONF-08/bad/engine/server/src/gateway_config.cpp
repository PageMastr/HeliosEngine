struct Cfg { Address listen = Address::ipv4(0, 0, 0, 0, 7005); };
auto target = parse(args.get("connect", "", "127.0.0.1:7006"));
