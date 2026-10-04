#include "helios/toolsfw/rpc.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <format>

#include "helios/core/version.h"
#include "helios/reflect/path.h"
#include "helios/toolsfw/framework.h"
#include "helios/toolsfw/json_util.h"

#include "platform/tf_os.h"

namespace helios::tf {

namespace ipc {
bool isValidEndpointName(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}
} // namespace ipc

i32 rpcErrorCode(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::NotFound: return rpcerr::kNotFound;
    case ErrorCode::InvalidArgument:
    case ErrorCode::ParseError:
    case ErrorCode::OutOfRange: return rpcerr::kInvalidParams;
    case ErrorCode::InvalidState:
    case ErrorCode::AlreadyExists:
    case ErrorCode::Busy: return rpcerr::kConflict;
    case ErrorCode::Timeout: return rpcerr::kTimeout;
    default: return rpcerr::kFailed;
    }
}

namespace {

ErrorCode errorCodeFromRpc(i64 code) noexcept {
    switch (code) {
    case rpcerr::kNotFound:
    case rpcerr::kMethodNotFound: return ErrorCode::NotFound;
    case rpcerr::kInvalidParams:
    case rpcerr::kInvalidRequest: return ErrorCode::InvalidArgument;
    case rpcerr::kParseError: return ErrorCode::ParseError;
    case rpcerr::kConflict: return ErrorCode::InvalidState;
    case rpcerr::kTimeout: return ErrorCode::Timeout;
    default: return ErrorCode::Unknown;
    }
}

std::string responseLine(std::string_view id, std::string_view resultJson) {
    std::string s = "{\"jsonrpc\":\"2.0\",\"id\":";
    s += id.empty() ? std::string_view("null") : id;
    s += ",\"result\":";
    s += resultJson.empty() ? std::string_view("null") : resultJson;
    s += "}\n";
    return s;
}

std::string errorLine(std::string_view id, i32 code, std::string_view message) {
    refl::JsonWriter w(refl::JsonStyle::Compact);
    w.beginObject();
    w.key("jsonrpc");
    w.string("2.0");
    w.key("id");
    if (id.empty()) {
        w.null();
    } else {
        w.raw(id);
    }
    w.key("error");
    w.beginObject();
    w.key("code");
    w.integer(code);
    w.key("message");
    w.string(message);
    w.endObject();
    w.endObject();
    return w.take() + "\n";
}

} // namespace

namespace detail {

/// One client connection. The reader thread parses requests into the server queue; the writer
/// thread drains `outbound`, so no other thread ever blocks on the client (07 §1.2: a stuck DCC
/// plug-in must not freeze the editor's main thread).
struct RpcConn {
    u64 id = 0;
    RpcServerLimits limits;
    std::unique_ptr<ipc::Connection> conn;
    std::thread reader;
    std::thread writer;
    std::atomic<bool> closed{false};

    std::mutex mutex;  ///< Guards everything below.
    std::condition_variable cv;
    std::deque<std::string> outbound;
    usize outboundBytes = 0;
    usize pending = 0;       ///< Requests queued or being handled (slots).
    usize pendingBytes = 0;

    /// Disconnects: wakes the reader, the writer and anyone waiting for a slot. Any thread.
    void close() {
        {
            std::lock_guard lock(mutex);
            closed.store(true);
            outbound.clear();
            outboundBytes = 0;
        }
        cv.notify_all();
        conn->shutdown();
    }

    /// Queues one line for the writer thread; never blocks. A client that has fallen
    /// maxOutboundBytes behind (it stopped reading) is disconnected.
    void send(std::string line) {
        bool tooSlow = false;
        {
            std::lock_guard lock(mutex);
            if (closed.load()) return;
            if (!outbound.empty() && outboundBytes + line.size() > limits.maxOutboundBytes) {
                tooSlow = true;
            } else {
                outboundBytes += line.size();
                outbound.push_back(std::move(line));
            }
        }
        if (tooSlow) {
            HELIOS_LOG_WARN(LogTools, "rpc: connection {} stopped reading its responses; disconnecting it", id);
            close();
            return;
        }
        cv.notify_all();
    }

    /// Takes a request slot for `bytes` of request text, waiting while the connection is at its
    /// limits (the reader then stops reading: backpressure). False once the connection closes.
    bool acquire(usize bytes, const std::atomic<bool>& stopping) {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] {
            return closed.load() || stopping.load() ||
                   (pending < limits.maxPendingRequests && (pending == 0 || pendingBytes + bytes <= limits.maxPendingBytes));
        });
        if (closed.load() || stopping.load()) return false;
        ++pending;
        pendingBytes += bytes;
        return true;
    }

    void release(usize bytes) {
        {
            std::lock_guard lock(mutex);
            --pending;
            pendingBytes -= bytes;
        }
        cv.notify_all();
    }

    void writerMain() {
        std::unique_lock lock(mutex);
        for (;;) {
            cv.wait(lock, [&] { return closed.load() || !outbound.empty(); });
            if (closed.load()) return;
            std::string line = std::move(outbound.front());
            outbound.pop_front();
            lock.unlock();
            const auto r = conn->write(line.data(), line.size());
            lock.lock();
            outboundBytes -= std::min(outboundBytes, line.size());
            cv.notify_all();  // a half-closed connection's reader waits for the last answer
            if (!r) break;
        }
        lock.unlock();
        close();
    }
};

struct RpcServerState {
    RpcServerLimits limits;
    std::unique_ptr<ipc::Listener> listener;
    std::thread acceptThread;
    mutable std::mutex mutex;
    std::map<u64, std::shared_ptr<RpcConn>> conns;
    std::deque<RpcRequest> queue;
    std::atomic<bool> stopping{false};
    u64 nextConn = 1;

    std::shared_ptr<RpcConn> find(u64 id) const {
        std::lock_guard lock(mutex);
        const auto it = conns.find(id);
        return it == conns.end() ? nullptr : it->second;
    }

    void readerMain(const std::shared_ptr<RpcConn>& c) {
        std::string buffer;
        usize scanned = 0;  // buffer[0, scanned) holds no '\n': each byte is searched once
        char chunk[64 * 1024];
        bool open = true;
        while (open && !stopping.load() && !c->closed.load()) {
            auto got = c->conn->read(chunk, sizeof(chunk));
            if (!got || *got == 0) break;
            buffer.append(chunk, *got);
            usize start = 0;
            for (usize nl = buffer.find('\n', scanned); nl != std::string::npos; nl = buffer.find('\n', start)) {
                std::string_view line(buffer.data() + start, nl - start);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                start = nl + 1;
                if (!line.empty() && !handleLine(c, line)) {
                    open = false;
                    break;
                }
            }
            if (!open) break;
            buffer.erase(0, start);
            scanned = buffer.size();
            if (buffer.size() > kRpcMaxMessageBytes) {
                c->send(errorLine({}, rpcerr::kInvalidRequest, "message too large"));
                break;
            }
        }
        // End of stream may be a half-close (`nc -N`, `socat`: the client shut its write side
        // after its last request and is still reading), and a final error line may be queued.
        // Answer every request read so far and write it out before disconnecting. The wait ends
        // early if the writer fails, the server stops, or the slow-reader cap closes the
        // connection; meanwhile the connection keeps its slot (maxConnections).
        {
            std::unique_lock lock(c->mutex);
            c->cv.wait(lock, [&] { return c->closed.load() || stopping.load() || (c->pending == 0 && c->outboundBytes == 0); });
        }
        c->close();
    }

    /// Parses one line and queues the request. False when the connection closed while waiting
    /// for a request slot.
    bool handleLine(const std::shared_ptr<RpcConn>& c, std::string_view line) {
        auto doc = refl::JsonDocument::parse(line, "<rpc>");
        if (!doc) {
            c->send(errorLine({}, rpcerr::kParseError, doc.error().message));
            return true;
        }
        const refl::JsonValue v = doc->root();
        const refl::JsonValue idValue = v.isObject() ? v.get("id") : refl::JsonValue();
        const std::string id = idValue.isValid() ? json::compact(idValue) : std::string();
        const auto method = json::getString(v, "method");
        if (!v.isObject() || !method || (idValue.isValid() && !idValue.isString() && !idValue.isNumber() && !idValue.isNull())) {
            c->send(errorLine(id, rpcerr::kInvalidRequest, "expected a JSON-RPC 2.0 request object"));
            return true;
        }
        RpcRequest req;
        req.method = std::string(*method);
        const refl::JsonValue params = v.get("params");
        if (params.isValid()) {
            if (!params.isObject()) {
                c->send(errorLine(id, rpcerr::kInvalidParams, "params must be an object"));
                return true;
            }
            req.params = json::compact(params);
        }
        req.id = id;
        req.connection = c->id;
        req.bytes = line.size();
        if (!c->acquire(req.bytes, stopping)) return false;
        std::lock_guard lock(mutex);
        queue.push_back(std::move(req));
        return true;
    }

    void acceptMain() {
        while (!stopping.load()) {
            auto conn = listener->accept();
            if (!conn) {
                if (conn.errorCode() == ErrorCode::Cancelled || stopping.load()) break;
                HELIOS_LOG_WARN(LogTools, "rpc accept: {}", conn.error());
                // A persistent failure (out of descriptors) must not spin this thread.
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            std::lock_guard lock(mutex);
            const usize live = static_cast<usize>(
                std::count_if(conns.begin(), conns.end(), [](const auto& p) { return !p.second->closed.load(); }));
            if (live >= limits.maxConnections) {
                // A fresh connection's socket buffer is empty, so this short write cannot block.
                const std::string line = errorLine({}, rpcerr::kFailed, std::format("too many connections (limit {})", limits.maxConnections));
                (void)(*conn)->write(line.data(), line.size());
                (*conn)->shutdown();
                continue;
            }
            auto c = std::make_shared<RpcConn>();
            c->conn = std::move(*conn);
            c->limits = limits;
            // The threads start under the lock: reap() and the destructor touch them only for
            // connections they found in `conns`, so they never see them half assigned.
            c->id = nextConn++;
            conns[c->id] = c;
            c->reader = std::thread([this, c] { readerMain(c); });
            c->writer = std::thread([c] { c->writerMain(); });
        }
    }

    /// Joins the threads of closed connections (owner thread).
    void reap() {
        std::vector<std::shared_ptr<RpcConn>> dead;
        {
            std::lock_guard lock(mutex);
            for (auto it = conns.begin(); it != conns.end();) {
                if (it->second->closed.load()) {
                    dead.push_back(it->second);
                    it = conns.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& c : dead) join(*c);
    }

    static void join(RpcConn& c) {
        c.close();
        if (c.reader.joinable()) c.reader.join();
        if (c.writer.joinable()) c.writer.join();
    }
};

/// One dispatched request: answers it once and gives its slot back to the connection.
struct RpcPending {
    std::weak_ptr<RpcConn> conn;
    std::string id;
    usize bytes = 0;
    std::atomic<bool> done{false};
    std::atomic<bool> released{false};

    ~RpcPending() { release(); }
    void release() {
        if (released.exchange(true)) return;
        if (auto c = conn.lock()) c->release(bytes);
    }
    void answer(std::string line) {
        if (auto c = conn.lock(); c && !id.empty()) c->send(std::move(line));
        release();
    }
};

} // namespace detail

// ---------------------------------------------------------------------------------------------
// RpcResponder
// ---------------------------------------------------------------------------------------------
void RpcResponder::result(std::string_view json) const {
    if (!m_pending || m_pending->done.exchange(true)) return;
    m_pending->answer(responseLine(m_pending->id, json));
}

void RpcResponder::error(i32 code, std::string_view message) const {
    if (!m_pending || m_pending->done.exchange(true)) return;
    m_pending->answer(errorLine(m_pending->id, code, message));
}

void RpcResponder::finish(const Result<std::string>& r) const {
    if (r) {
        result(*r);
    } else {
        error(r.error());
    }
}

bool RpcResponder::done() const noexcept {
    return !m_pending || m_pending->done.load();
}

// ---------------------------------------------------------------------------------------------
// RpcServer
// ---------------------------------------------------------------------------------------------
Result<std::unique_ptr<RpcServer>> RpcServer::start(std::string_view endpointName, const RpcServerLimits& limits) {
    if (limits.maxConnections == 0 || limits.maxPendingRequests == 0) {
        return Error{ErrorCode::InvalidArgument, "rpc: connection and request limits must be at least 1"};
    }
    HELIOS_TRY_ASSIGN(auto listener, ipc::Listener::listen(endpointName));
    std::unique_ptr<RpcServer> server(new RpcServer());
    server->m_endpoint = std::string(endpointName);
    server->m_state = std::make_shared<detail::RpcServerState>();
    server->m_state->limits = limits;
    server->m_state->listener = std::move(listener);
    detail::RpcServerState* st = server->m_state.get();
    st->acceptThread = std::thread([st] { st->acceptMain(); });
    server->registerMethod("rpc.methods", [s = server.get()](const RpcRequest&, const RpcResponder& r) {
        refl::JsonWriter w(refl::JsonStyle::Compact);
        w.beginArray();
        for (const auto& [name, entry] : s->m_methods) {
            w.beginObject();
            w.key("name");
            w.string(name);
            w.key("doc");
            w.string(entry.second);
            w.endObject();
        }
        w.endArray();
        r.result(w.take());
    }, "Lists the methods of this endpoint");
    return server;
}

RpcServer::~RpcServer() {
    if (!m_state) return;
    m_state->stopping.store(true);
    m_state->listener->shutdown();
    if (m_state->acceptThread.joinable()) m_state->acceptThread.join();
    std::vector<std::shared_ptr<detail::RpcConn>> conns;
    {
        std::lock_guard lock(m_state->mutex);
        for (auto& [id, c] : m_state->conns) conns.push_back(c);
        m_state->conns.clear();
    }
    for (auto& c : conns) detail::RpcServerState::join(*c);
}

const std::string& RpcServer::path() const noexcept {
    return m_state->listener->path();
}

void RpcServer::registerMethod(std::string name, RpcHandler handler, std::string doc) {
    m_methods[std::move(name)] = {std::move(handler), std::move(doc)};
}

usize RpcServer::pump() {
    std::deque<RpcRequest> work;
    {
        std::lock_guard lock(m_state->mutex);
        work.swap(m_state->queue);
    }
    for (const RpcRequest& req : work) {
        RpcResponder responder;
        responder.m_pending = std::make_shared<detail::RpcPending>();
        responder.m_pending->conn = m_state->find(req.connection);
        responder.m_pending->id = req.id;
        responder.m_pending->bytes = req.bytes;
        const auto it = m_methods.find(req.method);
        if (it == m_methods.end()) {
            responder.error(rpcerr::kMethodNotFound, std::format("unknown method '{}'", req.method));
            continue;
        }
        it->second.first(req, responder);
    }
    m_state->reap();
    return work.size();
}

void RpcServer::notifyAll(std::string_view method, std::string_view paramsJson) {
    std::string line = "{\"jsonrpc\":\"2.0\",\"method\":" + json::quote(method);
    if (!paramsJson.empty()) line += ",\"params\":" + std::string(paramsJson);
    line += "}\n";
    std::vector<std::shared_ptr<detail::RpcConn>> conns;
    {
        std::lock_guard lock(m_state->mutex);
        for (auto& [id, c] : m_state->conns) conns.push_back(c);
    }
    for (auto& c : conns) c->send(line);
}

usize RpcServer::queuedRequests() const {
    std::lock_guard lock(m_state->mutex);
    return m_state->queue.size();
}

usize RpcServer::connectionCount() const {
    std::lock_guard lock(m_state->mutex);
    return static_cast<usize>(std::count_if(m_state->conns.begin(), m_state->conns.end(),
                                            [](const auto& p) { return !p.second->closed.load(); }));
}

std::vector<std::string> RpcServer::methods() const {
    std::vector<std::string> out;
    for (const auto& [name, entry] : m_methods) out.push_back(name);
    return out;
}

// ---------------------------------------------------------------------------------------------
// RpcClient
// ---------------------------------------------------------------------------------------------
Result<std::unique_ptr<RpcClient>> RpcClient::connect(std::string_view endpointName, i32 timeoutMs) {
    HELIOS_TRY_ASSIGN(auto conn, ipc::connect(endpointName, timeoutMs));
    std::unique_ptr<RpcClient> c(new RpcClient());
    c->m_conn = std::move(conn);
    return c;
}

RpcClient::~RpcClient() {
    if (m_conn) m_conn->shutdown();
}

Result<std::string> RpcClient::readLine(i32 timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    for (;;) {
        const usize nl = m_buffer.find('\n');
        if (nl != std::string::npos) {
            std::string line = m_buffer.substr(0, nl);
            m_buffer.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            return line;
        }
        i32 wait = -1;
        if (timeoutMs >= 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) return Error{ErrorCode::Timeout, "rpc: timed out waiting for a response"};
            wait = static_cast<i32>(left);
        }
        char chunk[64 * 1024];
        HELIOS_TRY_ASSIGN(const usize got, m_conn->read(chunk, sizeof(chunk), wait));
        if (got == 0) return Error{ErrorCode::IoError, "rpc: connection closed"};
        m_buffer.append(chunk, got);
        if (m_buffer.size() > kRpcMaxMessageBytes) return Error{ErrorCode::LimitExceeded, "rpc: response too large"};
    }
}

Result<std::string> RpcClient::call(std::string_view method, std::string_view paramsJson, i32 timeoutMs) {
    const u64 id = m_nextId++;
    std::string line = std::format("{{\"jsonrpc\":\"2.0\",\"id\":{},\"method\":{}", id, json::quote(method));
    if (!paramsJson.empty()) line += ",\"params\":" + std::string(paramsJson);
    line += "}\n";
    HELIOS_TRY(m_conn->write(line.data(), line.size()));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        HELIOS_TRY_ASSIGN(const std::string msg, readLine(timeoutMs < 0 ? -1 : static_cast<i32>(std::max<i64>(left, 1))));
        HELIOS_TRY_ASSIGN(const refl::JsonDocument doc, refl::JsonDocument::parse(msg, "<rpc response>"));
        const refl::JsonValue v = doc.root();
        const refl::JsonValue rid = v.get("id");
        u64 got = 0;
        if (!rid.isValid() || rid.isNull()) {
            if (v.get("method").isValid()) {
                m_notifications.push_back(msg);
                continue;
            }
            if (const refl::JsonValue err = v.get("error"); err.isObject()) {
                return Error{errorCodeFromRpc(json::getInteger(err, "code").value_or(0)),
                             std::format("{}: {}", json::getInteger(err, "code").value_or(0),
                                         json::getString(err, "message").value_or(""))};
            }
            continue;
        }
        if (!rid.getU64(got) || got != id) continue;  // a late response of an abandoned call
        if (const refl::JsonValue err = v.get("error"); err.isObject()) {
            const i64 code = json::getInteger(err, "code").value_or(rpcerr::kFailed);
            return Error{errorCodeFromRpc(code), std::format("{}: {}", code, json::getString(err, "message").value_or(""))};
        }
        const refl::JsonValue result = v.get("result");
        return result.isValid() ? json::compact(result) : std::string("null");
    }
}

Result<void> RpcClient::notify(std::string_view method, std::string_view paramsJson) {
    std::string line = "{\"jsonrpc\":\"2.0\",\"method\":" + json::quote(method);
    if (!paramsJson.empty()) line += ",\"params\":" + std::string(paramsJson);
    line += "}\n";
    return m_conn->write(line.data(), line.size());
}

std::vector<std::string> RpcClient::takeNotifications() {
    return std::exchange(m_notifications, {});
}

// ---------------------------------------------------------------------------------------------
// ToolsFramework methods
// ---------------------------------------------------------------------------------------------
namespace {

void writeCommand(refl::JsonWriter& w, const CommandDesc& c) {
    w.beginObject();
    w.key("id");
    w.string(c.id);
    w.key("label");
    w.string(c.label);
    w.key("category");
    w.string(c.category);
    w.key("doc");
    w.string(c.doc);
    w.key("shortcut");
    w.string(c.shortcut);
    w.key("paletteOnly");
    w.boolean(c.paletteOnly);
    w.key("headless");
    w.boolean(c.headless);
    w.key("args");
    w.beginArray();
    for (const ArgDesc& a : c.args) {
        w.beginObject();
        w.key("name");
        w.string(a.name);
        w.key("type");
        w.string(argTypeName(a.type));
        w.key("required");
        w.boolean(a.required);
        w.key("doc");
        w.string(a.doc);
        w.endObject();
    }
    w.endArray();
    w.endObject();
}

Result<refl::JsonDocument> params(const RpcRequest& req) {
    return json::parseObject(req.params, req.method);
}

Result<const Document*> docParam(const Framework& fw, refl::JsonValue p) {
    const auto key = json::getString(p, "doc");
    if (!key) return Error{ErrorCode::InvalidArgument, "missing \"doc\""};
    const Document* d = fw.documents().find(*key);
    if (!d) return Error{ErrorCode::NotFound, std::format("no open document '{}'", *key)};
    return d;
}

/// Runs a command through the Rpc invoker and answers {"tx": "...", "result": ...}.
void invokeAndRespond(Framework& fw, std::string_view id, std::string_view args, const RpcResponder& r) {
    auto res = fw.invoker(Origin::Rpc).invoke(id, args);
    if (!res) {
        r.error(res.error());
        return;
    }
    refl::JsonWriter w(refl::JsonStyle::Compact);
    w.beginObject();
    w.key("tx");
    w.string(res->tx.isNull() ? std::string() : res->tx.toString());
    w.key("result");
    if (res->result.empty()) {
        w.null();
    } else {
        w.raw(res->result);
    }
    w.endObject();
    r.result(w.take());
}

std::string optionalDocArgs(refl::JsonValue p) {
    const auto doc = json::getString(p, "doc");
    return doc ? "{\"doc\":" + json::quote(*doc) + "}" : std::string();
}

} // namespace

void registerFrameworkRpc(RpcServer& server, Framework& fw) {
    server.registerMethod("helios.ping", [&fw](const RpcRequest&, const RpcResponder& r) {
        refl::JsonWriter w(refl::JsonStyle::Compact);
        w.beginObject();
        w.key("pid");
        w.unsignedInteger(os::currentProcessId());
        w.key("project");
        w.string(fw.config().project);
        w.key("session");
        w.string(fw.config().session);
        w.key("version");
        w.string(std::format("{}.{}.{}", HELIOS_VERSION_MAJOR, HELIOS_VERSION_MINOR, HELIOS_VERSION_PATCH));
        w.endObject();
        r.result(w.take());
    }, "Liveness check: {pid, project, session, version}");

    server.registerMethod("cmd.list", [&fw](const RpcRequest&, const RpcResponder& r) {
        refl::JsonWriter w(refl::JsonStyle::Compact);
        w.beginArray();
        for (const CommandDesc* c : fw.commands().commands()) writeCommand(w, *c);
        w.endArray();
        r.result(w.take());
    }, "Every registered command with its arguments");

    server.registerMethod("cmd.invoke", [&fw](const RpcRequest& req, const RpcResponder& r) {
        auto p = params(req);
        if (!p) return r.error(p.error());
        const auto id = json::getString(p->root(), "id");
        if (!id) return r.error(rpcerr::kInvalidParams, "missing \"id\"");
        const refl::JsonValue args = p->root().get("args");
        if (args.isValid() && !args.isObject()) return r.error(rpcerr::kInvalidParams, "\"args\" must be an object");
        invokeAndRespond(fw, *id, args.isValid() ? json::compact(args) : std::string(), r);
    }, "Runs a command {id, args?} with origin rpc: {tx, result}");

    server.registerMethod("cmd.canExecute", [&fw](const RpcRequest& req, const RpcResponder& r) {
        auto p = params(req);
        if (!p) return r.error(p.error());
        const auto id = json::getString(p->root(), "id");
        if (!id) return r.error(rpcerr::kInvalidParams, "missing \"id\"");
        const refl::JsonValue args = p->root().get("args");
        r.result(fw.invoker(Origin::Rpc).canExecute(*id, args.isValid() ? json::compact(args) : std::string()) ? "true" : "false");
    }, "Whether a command {id, args?} can run now");

    server.registerMethod("doc.list", [&fw](const RpcRequest&, const RpcResponder& r) {
        refl::JsonWriter w(refl::JsonStyle::Compact);
        w.beginArray();
        for (const Document* d : fw.documents().documents()) {
            w.beginObject();
            w.key("id");
            w.string(d->id().toString());
            w.key("name");
            w.string(d->name());
            w.key("file");
            w.string(d->relativePath());
            w.key("type");
            w.string(d->type().qualifiedName);
            w.key("revision");
            w.unsignedInteger(d->revision());
            w.key("dirty");
            w.boolean(d->dirty());
            w.key("destroyed");
            w.boolean(d->destroyed());
            w.endObject();
        }
        w.endArray();
        r.result(w.take());
    }, "Open documents: [{id, name, file, type, revision, dirty, destroyed}]");

    server.registerMethod("doc.get", [&fw](const RpcRequest& req, const RpcResponder& r) {
        auto p = params(req);
        if (!p) return r.error(p.error());
        auto d = docParam(fw, p->root());
        if (!d) return r.error(d.error());
        const std::string path(json::getString(p->root(), "path").value_or(""));
        auto v = refl::getJson((*d)->type(), (*d)->object(), path);
        if (!v) return r.error(v.error());
        r.result(*v);
    }, "Value at a property path {doc, path?} as JSON");

    server.registerMethod("doc.text", [&fw](const RpcRequest& req, const RpcResponder& r) {
        auto p = params(req);
        if (!p) return r.error(p.error());
        auto d = docParam(fw, p->root());
        if (!d) return r.error(d.error());
        r.result(json::quote((*d)->text()));
    }, "Canonical JSONC text of a document {doc}");

    server.registerMethod("doc.hash", [&fw](const RpcRequest& req, const RpcResponder& r) {
        auto p = params(req);
        if (!p) return r.error(p.error());
        auto d = docParam(fw, p->root());
        if (!d) return r.error(d.error());
        r.result(json::quote(hashHex((*d)->contentHash())));
    }, "XXH3-64 of a document's canonical text {doc}");

    server.registerMethod("doc.open", [&fw](const RpcRequest& req, const RpcResponder& r) {
        invokeAndRespond(fw, "doc.open", req.params, r);
    }, "Opens a record file {file}");

    server.registerMethod("doc.save", [&fw](const RpcRequest& req, const RpcResponder& r) {
        auto p = params(req);
        if (!p) return r.error(p.error());
        const std::string args = optionalDocArgs(p->root());
        invokeAndRespond(fw, args.empty() ? "doc.saveAll" : "doc.save", args, r);
    }, "Saves one document {doc} or all dirty ones");

    for (const bool redo : {false, true}) {
        server.registerMethod(redo ? "tx.redo" : "tx.undo", [&fw, redo](const RpcRequest& req, const RpcResponder& r) {
            auto p = params(req);
            if (!p) return r.error(p.error());
            invokeAndRespond(fw, redo ? "edit.redo" : "edit.undo", optionalDocArgs(p->root()), r);
        }, redo ? "Redo {doc?}" : "Undo {doc?}");
    }

    server.registerMethod("tx.history", [&fw](const RpcRequest& req, const RpcResponder& r) {
        auto p = params(req);
        if (!p) return r.error(p.error());
        std::optional<DocId> doc;
        if (const auto key = json::getString(p->root(), "doc")) {
            const Document* d = fw.documents().find(*key);
            if (!d) return r.error(rpcerr::kNotFound, "no such document");
            doc = d->id();
        }
        refl::JsonWriter w(refl::JsonStyle::Compact);
        w.beginArray();
        for (const HistoryEntry& e : fw.history()) {
            if (doc) {
                const auto docs = e.tx.documents();
                if (!containsDoc(docs, *doc)) continue;
            }
            w.beginObject();
            w.key("id");
            w.string(e.tx.id.toString());
            w.key("label");
            w.string(e.tx.label);
            w.key("origin");
            w.string(originName(e.tx.origin));
            w.key("undone");
            w.boolean(e.undone);
            w.key("ops");
            w.unsignedInteger(e.tx.ops.size());
            w.endObject();
        }
        w.endArray();
        r.result(w.take());
    }, "Undo history [{id, label, origin, undone, ops}] {doc?}");

    server.registerMethod("tx.log", [&fw](const RpcRequest& req, const RpcResponder& r) {
        auto p = params(req);
        if (!p) return r.error(p.error());
        const u64 since = static_cast<u64>(std::max<i64>(json::getInteger(p->root(), "since").value_or(0), 0));
        refl::JsonWriter w(refl::JsonStyle::Compact);
        w.beginArray();
        for (const Transaction& t : fw.log()) {
            if (t.id.lamport > since) t.writeJson(w);
        }
        w.endArray();
        r.result(w.take());
    }, "Committed transactions (do/undo/redo) with lamport > since {since?}");

    server.registerMethod("selection.get", [&fw](const RpcRequest&, const RpcResponder& r) {
        refl::JsonWriter w(refl::JsonStyle::Compact);
        w.beginArray();
        for (const ObjRef& ref : fw.selection().items()) {
            w.beginObject();
            w.key("doc");
            w.string(ref.doc.toString());
            w.key("guid");
            w.string(ref.guid.toString());
            w.key("path");
            w.string(ref.path);
            w.endObject();
        }
        w.endArray();
        r.result(w.take());
    }, "Current selection [{doc, guid, path}]");

    server.registerMethod("selection.set", [&fw](const RpcRequest& req, const RpcResponder& r) {
        invokeAndRespond(fw, "selection.set", req.params, r);
    }, "Selects a document {doc, path?}");
}

} // namespace helios::tf
