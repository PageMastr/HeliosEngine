// An option call whose parentheses #if branches unbalance never closes: it fails closed, and the read
// lines after it are still read.
auto listen = args.get("listen", "",
#if HELIOS_DEV
    pick("127.0.0.1:7777"
#else
    pick("0.0.0.0:7777"
#endif
    ));
void f() {}
auto connect = parseAddress("127.0.0.1:7000");
