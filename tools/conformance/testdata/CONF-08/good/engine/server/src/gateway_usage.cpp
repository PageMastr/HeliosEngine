// An option call in a raw string (usage text) is not a call: the option calls after it keep their own
// defaults.
constexpr std::string_view kUsage = R"(usage: spawn("gateway-1", {"--listen", "0.0.0.0:7777"}))";
auto listen = parseAddress(args.get("listen", "", "0.0.0.0:7777"), "--listen");
