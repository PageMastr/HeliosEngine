#pragma once
// Remote control (07 §1.2): JSON-RPC 2.0 over the local IPC endpoint of ipc.h
// (`\\.\pipe\helios-editor-<pid>`, a Unix socket on Linux). It exposes the command registry to DCC
// plug-ins, test harnesses (helios-uitest's `ui.*` methods) and `helios://` links.
//
// Framing: one JSON-RPC message per line (compact JSON, UTF-8, '\n'-terminated; "\r\n" is
// accepted). Batches are not supported. Requests larger than kRpcMaxMessageBytes close the
// connection.
//
// Threading: RpcServer accepts and reads on its own threads and queues requests; handlers run on
// the owner thread inside pump(). A handler may answer later (for example after some frames)
// through its RpcResponder, from the owner thread. RpcClient is single-threaded.

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "helios/core/result.h"
#include "helios/toolsfw/ipc.h"

namespace helios::tf {

class Framework;

inline constexpr usize kRpcMaxMessageBytes = 64u << 20;

/// JSON-RPC error codes (the -32000..-32099 range is Helios-defined).
namespace rpcerr {
inline constexpr i32 kParseError = -32700;
inline constexpr i32 kInvalidRequest = -32600;
inline constexpr i32 kMethodNotFound = -32601;
inline constexpr i32 kInvalidParams = -32602;
inline constexpr i32 kInternalError = -32603;
inline constexpr i32 kFailed = -32000;    ///< The operation failed (message says why).
inline constexpr i32 kNotFound = -32001;
inline constexpr i32 kConflict = -32002;  ///< InvalidState: undo conflicts, refused commands.
inline constexpr i32 kTimeout = -32003;
} // namespace rpcerr

/// JSON-RPC error code for a Helios ErrorCode.
i32 rpcErrorCode(ErrorCode code) noexcept;

struct RpcRequest {
    std::string method;
    /// Compact JSON of "params" ("" when absent).
    std::string params;
    /// Compact JSON of "id" ("" for a notification, which gets no response).
    std::string id;
    u64 connection = 0;
};

namespace detail {
struct RpcServerState;
struct RpcPending;
} // namespace detail

/// Completes one request. Copyable; the first result()/error() wins.
class RpcResponder {
public:
    RpcResponder() = default;
    /// `json` is the compact JSON of the result value ("null" when empty).
    void result(std::string_view json) const;
    void error(i32 code, std::string_view message) const;
    void error(const Error& error) const { this->error(rpcErrorCode(error.code), error.toString()); }
    /// result() or error() from a Result<std::string> of compact JSON.
    void finish(const Result<std::string>& r) const;
    bool done() const noexcept;

private:
    friend class RpcServer;
    std::shared_ptr<detail::RpcPending> m_pending;
};

using RpcHandler = std::function<void(const RpcRequest&, const RpcResponder&)>;

class RpcServer {
public:
    /// Starts listening on `endpointName` (see ipc::endpointPath).
    static Result<std::unique_ptr<RpcServer>> start(std::string_view endpointName);
    ~RpcServer();
    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    /// Adds or replaces a method. `doc` is listed by the built-in `rpc.methods`.
    void registerMethod(std::string name, RpcHandler handler, std::string doc = {});
    /// Dispatches every queued request on the calling (owner) thread. Returns how many ran.
    usize pump();
    /// Sends a JSON-RPC notification to every connected client (any thread).
    void notifyAll(std::string_view method, std::string_view paramsJson);

    const std::string& endpoint() const noexcept { return m_endpoint; }
    const std::string& path() const noexcept;
    usize connectionCount() const;
    std::vector<std::string> methods() const;

private:
    RpcServer() = default;
    std::string m_endpoint;
    std::shared_ptr<detail::RpcServerState> m_state;
    std::map<std::string, std::pair<RpcHandler, std::string>, std::less<>> m_methods;
};

class RpcClient {
public:
    /// Connects, retrying for up to `timeoutMs` while the server starts.
    static Result<std::unique_ptr<RpcClient>> connect(std::string_view endpointName, i32 timeoutMs = 10000);
    ~RpcClient();
    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    /// Calls `method` and waits for its response. Returns the result's compact JSON; a JSON-RPC
    /// error becomes an Error (code mapped back from the RPC code, message "<code>: <message>").
    Result<std::string> call(std::string_view method, std::string_view paramsJson = {}, i32 timeoutMs = 60000);
    /// Sends a notification (no response).
    Result<void> notify(std::string_view method, std::string_view paramsJson = {});
    /// Notifications received while waiting for responses (compact JSON messages).
    std::vector<std::string> takeNotifications();

private:
    RpcClient() = default;
    Result<std::string> readLine(i32 timeoutMs);
    std::unique_ptr<ipc::Connection> m_conn;
    std::string m_buffer;
    std::vector<std::string> m_notifications;
    u64 m_nextId = 1;
};

/// Registers the ToolsFramework methods on `server` (they run on the owner thread, through the
/// framework's Rpc invoker):
///   helios.ping, rpc.methods,
///   cmd.list, cmd.invoke {id, args?}, cmd.canExecute {id, args?},
///   doc.list, doc.get {doc, path?}, doc.text {doc}, doc.hash {doc}, doc.open {file},
///   doc.save {doc?}, tx.undo {doc?}, tx.redo {doc?}, tx.history {doc?}, tx.log {since?},
///   selection.get, selection.set {refs}.
void registerFrameworkRpc(RpcServer& server, Framework& framework);

} // namespace helios::tf
