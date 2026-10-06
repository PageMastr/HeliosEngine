// The same call inside a function and a namespace: the braces that close them are not its closer.
namespace helios::server {
int runProbe(const ProcessArgs& args) {
    auto listen = args.get("listen", "",
#if HELIOS_DEV
        pick("127.0.0.1:7777"
#else
        pick("0.0.0.0:7777"
#endif
        ));
    auto connect = parseAddress("127.0.0.1:7000");
    return 0;
}
}
