// Nor is a `)` after a `;` outside any bracket (an if statement's initializer), or a `}` with no `;` before
// it (a braced initializer). The read-line literals between each call and that closer are still read.
namespace helios::server {
int runIfInit(const ProcessArgs& args) {
    if (auto bind = args.get("listen", "",
#if HELIOS_DEV
            pick("127.0.0.1:7777"
#else
            pick("0.0.0.0:7777"
#endif
            )); bind != listenFallback("127.0.0.1:7001")) {
        return 1;
    }
    return 0;
}
const GatewayOptions kOptions{
    args.get("listen", "",
#if HELIOS_DEV
        pick("127.0.0.1:7777"
#else
        pick("0.0.0.0:7777"
#endif
        )),
    connectTarget("127.0.0.1:7002")
};
}
