// Remote control: JSON-RPC 2.0 over the local socket / named pipe (07 §1.2).

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <format>
#include <thread>

#include "helios/core/process.h"
#include "helios/toolsfw/json_util.h"
#include "helios/toolsfw/rpc.h"
#include "test_util.h"

using namespace helios;
using namespace helios::tf;
using namespace helios::tf::test;

// REQUIRE throws, which would terminate a client thread; this checks and returns instead.
#define TREQUIRE(expr)                        \
    do {                                      \
        const bool treqOk_ = static_cast<bool>(expr); \
        CHECK_MESSAGE(treqOk_, #expr);        \
        if (!treqOk_) return;                 \
    } while (false)

namespace {

std::string endpoint(std::string_view tag) {
    static std::atomic<int> counter{0};
    return std::format("helios-test-{}-{}-{}", tag, static_cast<u32>(std::hash<std::string>{}(std::to_string(
                                                          std::chrono::steady_clock::now().time_since_epoch().count())) & 0xFFFF),
                       counter++);
}

/// Runs the server's pump on this thread while `fn` runs the client on another.
template <class Fn>
void withServer(RpcServer& server, Fn&& fn) {
    std::atomic<bool> done{false};
    std::thread client([&] {
        fn();
        done = true;
    });
    while (!done) {
        server.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    client.join();
    server.pump();
}

TEST_CASE("rpc: endpoint names are validated") {
    CHECK(ipc::isValidEndpointName("helios-editor-1234"));
    CHECK_FALSE(ipc::isValidEndpointName(""));
    CHECK_FALSE(ipc::isValidEndpointName("a/b"));
    CHECK_FALSE(ipc::isValidEndpointName(std::string(65, 'a')));
    CHECK(RpcServer::start("bad name").errorCode() == ErrorCode::InvalidArgument);
    CHECK(ipc::endpointPath("x").find("x") != std::string::npos);
}

TEST_CASE("rpc: framework methods over a real endpoint, stamped with origin rpc") {
    Fixture f("rpc_fw");
    auto server = RpcServer::start(endpoint("fw"));
    REQUIRE(server);
    registerFrameworkRpc(**server, *f.fw);
    CHECK(RpcServer::start((*server)->endpoint()).errorCode() == ErrorCode::AlreadyExists);
    withServer(**server, [&] {
        auto c = RpcClient::connect((*server)->endpoint(), 5000);
        TREQUIRE(c);
        auto ping = (*c)->call("helios.ping");
        TREQUIRE(ping);
        CHECK(ping->find("\"project\":\"test-project\"") != std::string::npos);
        auto list = (*c)->call("cmd.list");
        TREQUIRE(list);
        CHECK(list->find("\"doc.setProperty\"") != std::string::npos);
        auto set = (*c)->call("cmd.invoke", R"({"id": "doc.setProperty", "args": {"doc": "hull/frigate", "path": "mass", "value": 15000}})");
        TREQUIRE(set);
        CHECK(set->find("\"tx\":\"tester:1\"") != std::string::npos);
        auto get = (*c)->call("doc.get", R"({"doc": "hull/frigate", "path": "mass"})");
        TREQUIRE(get);
        CHECK(*get == "15000");
        auto hash = (*c)->call("doc.hash", R"({"doc": "hull/frigate"})");
        TREQUIRE(hash);
        CHECK(hash->size() == 18);
        auto docs = (*c)->call("doc.list");
        TREQUIRE(docs);
        CHECK(docs->find("\"dirty\":true") != std::string::npos);
        auto text = (*c)->call("doc.text", R"({"doc": "hull/frigate"})");
        TREQUIRE(text);
        CHECK(text->find("15000") != std::string::npos);
        auto hist = (*c)->call("tx.history");
        TREQUIRE(hist);
        CHECK(hist->find("\"origin\":\"rpc\"") != std::string::npos);
        TREQUIRE((*c)->call("tx.undo"));
        auto log = (*c)->call("tx.log", R"({"since": 1})");
        TREQUIRE(log);
        CHECK(log->find("\"kind\":\"undo\"") != std::string::npos);
        TREQUIRE((*c)->call("selection.set", R"({"doc": "hull/frigate"})"));
        auto sel = (*c)->call("selection.get");
        TREQUIRE(sel);
        CHECK(sel->find(f.frigate.toString()) != std::string::npos);
        auto methods = (*c)->call("rpc.methods");
        TREQUIRE(methods);
        CHECK(methods->find("\"cmd.invoke\"") != std::string::npos);
    });
    REQUIRE(f.fw->log().size() == 2);
    CHECK(f.fw->log()[0].origin == Origin::Rpc);
    CHECK(f.fw->log()[1].origin == Origin::Rpc);
    CHECK(f.get("mass") == "12000");
}

TEST_CASE("rpc: errors follow JSON-RPC 2.0") {
    Fixture f("rpc_errors");
    auto server = RpcServer::start(endpoint("err"));
    REQUIRE(server);
    registerFrameworkRpc(**server, *f.fw);
    withServer(**server, [&] {
        auto c = RpcClient::connect((*server)->endpoint(), 5000);
        TREQUIRE(c);
        auto unknown = (*c)->call("no.such.method");
        CHECK(unknown.errorCode() == ErrorCode::NotFound);
        CHECK(unknown.error().message.starts_with("-32601"));
        CHECK((*c)->call("cmd.invoke", R"({"args": {}})").errorCode() == ErrorCode::InvalidArgument);
        CHECK((*c)->call("cmd.invoke", R"({"id": "doc.setProperty", "args": {"doc": "hull/frigate", "path": "mass", "value": 1}})")
                  .errorCode() == ErrorCode::InvalidArgument);  // @range
        CHECK((*c)->call("doc.get", R"({"doc": "missing"})").errorCode() == ErrorCode::NotFound);
        CHECK((*c)->call("tx.undo").errorCode() == ErrorCode::InvalidState);
    });
    // Raw protocol violations.
    withServer(**server, [&] {
        auto conn = ipc::connect((*server)->endpoint(), 5000);
        TREQUIRE(conn);
        const std::string lines = "this is not json\r\n[1,2]\n{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"helios.ping\",\"params\":[1]}\n";
        TREQUIRE((*conn)->write(lines.data(), lines.size()));
        std::string got;
        char buf[4096];
        while (std::count(got.begin(), got.end(), '\n') < 3) {
            auto n = (*conn)->read(buf, sizeof(buf), 5000);
            TREQUIRE(n);
            TREQUIRE(*n > 0);
            got.append(buf, *n);
        }
        CHECK(got.find("-32700") != std::string::npos);
        CHECK(got.find("-32600") != std::string::npos);
        CHECK(got.find("\"id\":7,\"error\":{\"code\":-32602") != std::string::npos);
    });
}

TEST_CASE("rpc: deferred responses, notifications and several clients") {
    auto server = RpcServer::start(endpoint("defer"));
    REQUIRE(server);
    std::vector<RpcResponder> pending;
    int pumpsSince = 0;
    (*server)->registerMethod("test.later", [&](const RpcRequest& req, const RpcResponder& r) {
        CHECK(req.params == "{\"n\":3}");
        pending.push_back(r);
        pumpsSince = 0;
    });
    (*server)->registerMethod("test.echo", [](const RpcRequest& req, const RpcResponder& r) { r.result(req.params); });
    std::atomic<bool> done{false};
    std::thread clients([&] {
        [&] {
        auto a = RpcClient::connect((*server)->endpoint(), 5000);
        auto b = RpcClient::connect((*server)->endpoint(), 5000);
        TREQUIRE(a);
        TREQUIRE(b);
        TREQUIRE((*b)->notify("test.echo", "{}"));  // no response expected
        auto later = (*a)->call("test.later", R"({"n": 3})", 10000);
        TREQUIRE(later);
        CHECK(*later == "\"finally\"");
        auto echo = (*b)->call("test.echo", R"({"x": [1, 2]})");
        TREQUIRE(echo);
        CHECK(*echo == R"({"x":[1,2]})");
        CHECK((*a)->takeNotifications().size() == 1);
        }();
        done = true;
    });
    bool notified = false;
    while (!done) {
        (*server)->pump();
        if (!pending.empty() && ++pumpsSince > 20) {
            if (!notified) {
                (*server)->notifyAll("test.event", R"({"frame": 20})");
                notified = true;
            }
            pending.front().result("\"finally\"");
            pending.front().result("\"twice\"");  // ignored
            CHECK(pending.front().done());
            pending.clear();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    clients.join();
}

TEST_CASE("rpc: a server that goes away closes its clients' calls") {
    auto server = RpcServer::start(endpoint("gone"));
    REQUIRE(server);
    auto c = RpcClient::connect((*server)->endpoint(), 5000);
    REQUIRE(c);
    const std::string name = (*server)->endpoint();
    server->reset();
    auto r = (*c)->call("helios.ping", {}, 2000);
    CHECK_FALSE(r);
    // The name is free again.
    auto again = RpcServer::start(name);
    CHECK(again);
    CHECK(RpcClient::connect("helios-test-nobody-listens", 50).errorCode() == ErrorCode::Timeout);
}

/// `n` pipelined JSON-RPC requests of `method` with `params`, one per line.
std::string pipelined(int n, std::string_view method, std::string_view params) {
    std::string out;
    for (int i = 0; i < n; ++i) {
        out += std::format(R"({{"jsonrpc":"2.0","id":{},"method":"{}","params":{}}})", i + 1, method, params);
        out += '\n';
    }
    return out;
}

/// Pumps `server` on this thread until `done()` or `timeout`; returns how many requests ran.
template <class Pred>
usize pumpUntil(RpcServer& server, Pred&& done, std::chrono::milliseconds timeout) {
    usize ran = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        ran += server.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ran;
}

TEST_CASE("rpc: a client that stops reading never blocks the owner thread") {
    Fixture f("rpc_stall");
    auto server = RpcServer::start(endpoint("stall"));
    REQUIRE(server);
    registerFrameworkRpc(**server, *f.fw);
    auto conn = ipc::connect((*server)->endpoint(), 5000);
    REQUIRE(conn);
    // 2,000 pipelined doc.text requests: about 3 MB of responses, far more than the socket or pipe
    // buffers hold, and the client never reads one of them.
    constexpr usize kRequests = 2000;
    const std::string batch = pipelined(static_cast<int>(kRequests), "doc.text", R"({"doc":"hull/frigate"})");
    std::thread writer([&] { (void)(*conn)->write(batch.data(), batch.size()); });
    // pump() runs on a helper thread, so a pump() that blocks fails this check instead of hanging it.
    std::atomic<usize> handled{0};
    std::atomic<bool> stop{false};
    std::thread owner([&] {
        while (!stop.load() && handled.load() < kRequests) {
            handled += (*server)->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (handled.load() < kRequests && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(handled.load() == kRequests);
    stop = true;
    (*conn)->shutdown();
    owner.join();
    writer.join();
}

TEST_CASE("rpc: limits bound connections, queued requests and unread responses") {
    RpcServerLimits limits;
    limits.maxConnections = 2;
    limits.maxPendingRequests = 4;
    limits.maxOutboundBytes = 64 * 1024;
    auto server = RpcServer::start(endpoint("limits"), limits);
    REQUIRE(server);
    const std::string big = "\"" + std::string(16 * 1024, 'x') + "\"";
    (*server)->registerMethod("test.big", [&](const RpcRequest&, const RpcResponder& r) { r.result(big); });
    CHECK(RpcServer::start(endpoint("nolimits"), RpcServerLimits{.maxConnections = 0}).errorCode() == ErrorCode::InvalidArgument);

    auto a = ipc::connect((*server)->endpoint(), 5000);
    auto b = ipc::connect((*server)->endpoint(), 5000);
    REQUIRE(a);
    REQUIRE(b);
    pumpUntil(**server, [&] { return (*server)->connectionCount() == 2; }, std::chrono::seconds(10));
    REQUIRE((*server)->connectionCount() == 2);

    // A third client gets an error line, then end of stream.
    {
        auto c = ipc::connect((*server)->endpoint(), 5000);
        REQUIRE(c);
        std::string got;
        char buf[512];
        for (int i = 0; i < 100; ++i) {
            auto n = (*c)->read(buf, sizeof(buf), 100);
            if (n && *n == 0) break;
            if (n) got.append(buf, *n);
        }
        CHECK(got.find("too many connections") != std::string::npos);
        CHECK((*server)->connectionCount() == 2);
    }

    // Backpressure: with nobody pumping, at most maxPendingRequests requests of a are queued; the
    // rest wait in the socket (and then in the client's write).
    const std::string batch = pipelined(100, "test.big", "{}");
    std::thread writer([&] { (void)(*a)->write(batch.data(), batch.size()); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK((*server)->queuedRequests() <= limits.maxPendingRequests);
    CHECK((*server)->queuedRequests() > 0);

    // a never reads its 1.6 MB of responses: once they back up past the socket and
    // maxOutboundBytes, a is disconnected, and pump() keeps running meanwhile.
    pumpUntil(**server, [&] { return (*server)->connectionCount() == 1; }, std::chrono::seconds(30));
    CHECK((*server)->connectionCount() == 1);
    (*a)->shutdown();
    writer.join();
    // The freed slot admits a new client.
    auto d = RpcClient::connect((*server)->endpoint(), 5000);
    REQUIRE(d);
    std::atomic<bool> ok{false};
    withServer(**server, [&] {
        auto r = (*d)->call("rpc.methods", {}, 10000);
        ok = r.ok();
    });
    CHECK(ok.load());
}

} // namespace
