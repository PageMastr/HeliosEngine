// An option call in a raw string (usage text) is not a call: the option call after it is still checked,
// with its own default, on its own line.
constexpr std::string_view kUsage = R"(usage: spawn("gateway-1", {"--listen", "0.0.0.0:7777"}))";
auto listen = parseAddress(args.get("listen", "", "127.0.0.1:7000"), "--listen");
