// A block's braces end a read statement: a lambda's body, a constructor's body after its initializer list,
// a labelled block, a body after a trailing return type, a struct with a templated base, a function-try
// block, a do statement and a returned lambda. None of these addresses is the gateway's.
void hooks(Server& server) {
    server.onConnect(
        [&] {
            metrics.push("10.0.0.9:9100");
        });
}
Probe::Probe(Net& n)
    : connect_{n} {
    metrics = "10.0.0.9:9100";
}
void route(Mode mode) {
    switch (mode) {
    case Mode::Connect: {
        metrics = "10.0.0.9:9100";
    }
    }
}
auto connectTrunk() -> net::Address {
    return net::Address::ipv4(10, 0, 0, 5, 7100);
}
struct ConnectStats : Counters<int> {
    std::string metrics = "10.0.0.9:9100";
};
void connectProbe() try {
    metrics = "10.0.0.9:9100";
} catch (...) {
}
void retry(bool connect) {
    if (connect) do {
        metrics = "10.0.0.9:9100";
    } while (false);
}
auto onConnect() {
    return [connect_] {
        metrics.push("10.0.0.9:9100");
    };
}
// `;` ends a statement inside a lambda's body in a call's parentheses, also after an inner lambda closes.
void spawn() {
    run([&] { connect_ = true; metrics =
        "10.0.0.9:9100"; });
    run([&] {
        each([&] { step(); });
        connect_ = true; metrics =
            "10.0.0.9:9100";
    });
}
