// The gateway listens on 7777; its trunk to the cells uses other ports, which are not the game port.
auto listen = parse(args.get("listen", "", "127.0.0.1:7777"));
auto cell = parse(args.get("cell", "", "127.0.0.1:7810"));
