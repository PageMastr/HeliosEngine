// A read line is found by the words of its names, not only the whole words listen and connect: listenAddress,
// listen_address, clientListen and a kListenAddr constant (reported where it is declared and where it is used).
net::Address listenAddress = net::Address::ipv4(127, 0, 0, 1, 7050);
net::Address listen_address = net::Address::ipv4(127, 0, 0, 1, 7051);
net::Address clientListen = net::Address::ipv4(127, 0, 0, 1, 7052);
constexpr const char* kListenAddr = "0.0.0.0:7053";
auto parsed = net::Address::parse(kListenAddr);
