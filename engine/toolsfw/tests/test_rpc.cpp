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

} // namespace
