#include "helios/server/nats_bus.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

#include <nats.h>

#include "helios/core/jobs.h"
#include "helios/core/time.h"
#include "server_log.h"

namespace helios::server {

namespace {
/// What a subscription's message handler reaches through nats.c's closure pointer. nats.c may
/// still be running (or about to run) the handler on its delivery thread after
/// natsSubscription_Unsubscribe() returns, so the closure is shared with the subscription's
/// on-complete callback, which nats.c calls once the subscription is closed *and* its last
/// handler call has returned. unsubscribe() waits for that before the caller may free what the
/// handler uses (unless it is called from inside the handler itself).
struct SubscriptionClosure {
    BusMessageFn fn;
    std::mutex mutex;
    std::condition_variable cv;
    bool complete = false;
    std::atomic<std::thread::id> dispatching{}; ///< The delivery thread while inside `fn`.
};

BusMessage fromNats(natsMsg* msg) {
    BusMessage out;
    if (const char* s = natsMsg_GetSubject(msg)) out.subject = s;
    if (const char* r = natsMsg_GetReply(msg)) out.reply = r;
    const char* data = natsMsg_GetData(msg);
    const int len = natsMsg_GetDataLength(msg);
    if (data && len > 0) out.data.assign(reinterpret_cast<const u8*>(data), reinterpret_cast<const u8*>(data) + len);
    const char** keys = nullptr;
    int count = 0;
    if (natsMsgHeader_Keys(msg, &keys, &count) == NATS_OK) {
        for (int i = 0; i < count; ++i) {
            const char* value = nullptr;
            if (natsMsgHeader_Get(msg, keys[i], &value) == NATS_OK && value) out.headers.emplace_back(keys[i], value);
        }
        std::free(static_cast<void*>(keys));
    }
    return out;
}

void natsMessageThunk(natsConnection*, natsSubscription*, natsMsg* msg, void* closure) {
    auto* c = static_cast<SubscriptionClosure*>(closure);
    if (!msg) return; // subscription timeout notifications (not used)
    const BusMessage m = fromNats(msg);
    natsMsg_Destroy(msg);
    c->dispatching.store(std::this_thread::get_id());
    c->fn(m);
    c->dispatching.store(std::thread::id{});
}

/// nats.c's on-complete callback: owns one reference to the closure.
void natsCompleteThunk(void* closure) {
    auto* ref = static_cast<std::shared_ptr<SubscriptionClosure>*>(closure);
    {
        std::lock_guard lock((*ref)->mutex);
        (*ref)->complete = true;
    }
    (*ref)->cv.notify_all();
    delete ref;
}

/// How long unsubscribe() waits for an in-flight handler before giving up (the closure then stays
/// alive, owned by the on-complete callback, so nothing is freed under the handler).
constexpr auto kUnsubscribeWait = std::chrono::seconds(5);

/// Closes `sub`, waits until its handler can no longer run (unless called from that handler) and
/// destroys it. The caller's reference to the closure may be dropped afterwards.
void closeSubscription(natsSubscription* sub, const std::shared_ptr<SubscriptionClosure>& closure) {
    if (!sub) return;
    natsSubscription_Unsubscribe(sub);
    if (closure && closure->dispatching.load() != std::this_thread::get_id()) {
        std::unique_lock lock(closure->mutex);
        if (!closure->cv.wait_for(lock, kUnsubscribeWait, [&] { return closure->complete; }))
            HELIOS_LOG_WARN(LogServer, "NATS subscription did not complete within {} s of unsubscribe",
                            std::chrono::duration_cast<std::chrono::seconds>(kUnsubscribeWait).count());
    }
    natsSubscription_Destroy(sub);
}
} // namespace

struct NatsBus::Impl {
    natsOptions* options = nullptr;
    natsConnection* conn = nullptr;
    std::atomic<bool> connected{false};
    std::atomic<bool> closing{false};
    std::mutex subMutex;
    struct Sub {
        natsSubscription* sub = nullptr;
        std::shared_ptr<SubscriptionClosure> closure;
    };
    std::map<u64, Sub> subs;
    u64 nextSub = 1;
    std::unique_ptr<jobs::BackgroundPool> pool;
    std::string name;
    // nats.c runs the connection callbacks on its own async-callback thread, and Close() posts the
    // disconnected and closed callbacks there (in that order) to run later. The closed callback is
    // the last one that can touch this object, so the destructor waits for it.
    std::mutex closedMutex;
    std::condition_variable closedCv;
    bool closed = false;

    static void onConnected(natsConnection*, void* closure) {
        auto* self = static_cast<Impl*>(closure);
        self->connected.store(true);
        HELIOS_LOG_INFO(LogServer, "NATS '{}' connected", self->name);
    }
    static void onDisconnected(natsConnection*, void* closure) {
        auto* self = static_cast<Impl*>(closure);
        self->connected.store(false);
        if (!self->closing.load()) HELIOS_LOG_WARN(LogServer, "NATS '{}' disconnected; reconnecting", self->name);
    }
    static void onReconnected(natsConnection*, void* closure) {
        auto* self = static_cast<Impl*>(closure);
        self->connected.store(true);
        HELIOS_LOG_INFO(LogServer, "NATS '{}' reconnected", self->name);
    }
    static void onClosed(natsConnection*, void* closure) {
        auto* self = static_cast<Impl*>(closure);
        self->connected.store(false);
        std::lock_guard lock(self->closedMutex);
        self->closed = true;
        self->closedCv.notify_all(); // under the lock: the destructor may free the object right after
    }

    ~Impl() {
        closing.store(true);
        std::map<u64, Sub> open;
        {
            std::lock_guard lock(subMutex);
            open.swap(subs);
        }
        // Outside the lock: a handler still running may call subscribe()/unsubscribe().
        for (auto& [id, s] : open) closeSubscription(s.sub, s.closure);
        open.clear();
        if (conn) {
            natsConnection_Close(conn); // wakes blocked requests, posts the disconnected/closed callbacks
            std::unique_lock lock(closedMutex);
            if (!closedCv.wait_for(lock, std::chrono::seconds(5), [&] { return closed; }))
                HELIOS_LOG_WARN(LogServer, "NATS '{}': no closed callback within 5 s", name);
        }
        pool.reset(); // joins request threads (they finish quickly now)
        if (conn) natsConnection_Destroy(conn);
        if (options) natsOptions_Destroy(options);
    }
};

NatsBus::NatsBus(std::unique_ptr<Impl> impl) : m(std::move(impl)) {}
NatsBus::~NatsBus() = default;

Result<std::unique_ptr<NatsBus>> NatsBus::connect(const NatsBusConfig& config) {
    auto impl = std::make_unique<Impl>();
    impl->name = config.name;
    auto fail = [](natsStatus s, std::string_view what) {
        return makeError(ErrorCode::IoError, "NATS {}: {}", what, natsStatus_GetText(s));
    };
    natsStatus s = natsOptions_Create(&impl->options);
    if (s != NATS_OK) return fail(s, "options");
    natsOptions* o = impl->options;
    if ((s = natsOptions_SetURL(o, config.url.c_str())) != NATS_OK) return fail(s, "url");
    if (!config.user.empty() && (s = natsOptions_SetUserInfo(o, config.user.c_str(), config.password.c_str())) != NATS_OK)
        return fail(s, "user info");
    natsOptions_SetName(o, config.name.c_str());
    natsOptions_SetTimeout(o, config.connectTimeoutMs);
    natsOptions_SetAllowReconnect(o, true);
    natsOptions_SetMaxReconnect(o, -1);
    natsOptions_SetReconnectWait(o, config.reconnectWaitMs);
    natsOptions_SetDisconnectedCB(o, &Impl::onDisconnected, impl.get());
    natsOptions_SetReconnectedCB(o, &Impl::onReconnected, impl.get());
    natsOptions_SetClosedCB(o, &Impl::onClosed, impl.get());
    if ((s = natsOptions_SetRetryOnFailedConnect(o, true, &Impl::onConnected, impl.get())) != NATS_OK)
        return fail(s, "retry option");
    s = natsConnection_Connect(&impl->conn, o);
    if (s == NATS_OK) {
        impl->connected.store(true);
    } else if (s == NATS_NOT_YET_CONNECTED) {
        // Keeps trying in the background; wait a little for a quick start.
        const u64 deadline = monotonicNanos() + static_cast<u64>(std::max<i64>(config.initialConnectWaitMs, 0)) * 1'000'000ull;
        while (!impl->connected.load() && monotonicNanos() < deadline) sleepMillis(10);
        if (!impl->connected.load())
            HELIOS_LOG_WARN(LogServer, "NATS {} not reachable yet; retrying in the background", config.url);
    } else {
        return fail(s, "connect to " + config.url);
    }
    impl->pool = std::make_unique<jobs::BackgroundPool>(std::max<u32>(1, config.requestThreads), "NatsRpc",
                                                        ThreadPriority::Normal);
    return std::unique_ptr<NatsBus>(new NatsBus(std::move(impl)));
}

bool NatsBus::isConnected() const {
    return m->connected.load() && natsConnection_Status(m->conn) == NATS_CONN_STATUS_CONNECTED;
}

void NatsBus::request(std::string subject, std::vector<u8> payload, BusHeaders headers, std::chrono::milliseconds timeout,
                      BusReplyFn onReply) {
    Impl* impl = m.get();
    impl->pool->run([impl, subject = std::move(subject), payload = std::move(payload), headers = std::move(headers),
                     timeout, onReply = std::move(onReply)]() mutable {
        BusReply out;
        if (impl->closing.load() || !impl->conn) {
            out.status = BusStatus::Disconnected;
            out.transportError = "bus closed";
            onReply(std::move(out));
            return;
        }
        natsMsg* msg = nullptr;
        natsStatus s = natsMsg_Create(&msg, subject.c_str(), nullptr, reinterpret_cast<const char*>(payload.data()),
                                      static_cast<int>(payload.size()));
        bool hasDeadline = false;
        for (const auto& [k, v] : headers) {
            hasDeadline |= k == kHeaderDeadlineMs;
            if (s == NATS_OK) s = natsMsgHeader_Set(msg, k.c_str(), v.c_str());
        }
        if (s == NATS_OK && !hasDeadline)
            s = natsMsgHeader_Set(msg, std::string(kHeaderDeadlineMs).c_str(), std::to_string(timeout.count()).c_str());
        natsMsg* reply = nullptr;
        if (s == NATS_OK) s = natsConnection_RequestMsg(&reply, impl->conn, msg, timeout.count());
        natsMsg_Destroy(msg);
        switch (s) {
        case NATS_OK:
            out.status = BusStatus::Ok;
            out.message = fromNats(reply);
            natsMsg_Destroy(reply);
            break;
        case NATS_TIMEOUT: out.status = BusStatus::Timeout; break;
        case NATS_NO_RESPONDERS: out.status = BusStatus::NoResponders; break;
        case NATS_CONNECTION_CLOSED:
        case NATS_CONNECTION_DISCONNECTED:
        case NATS_NOT_YET_CONNECTED: out.status = BusStatus::Disconnected; break;
        default: out.status = BusStatus::Error; break;
        }
        if (s != NATS_OK) out.transportError = natsStatus_GetText(s);
        onReply(std::move(out));
    });
}

Result<u64> NatsBus::subscribe(std::string subject, BusMessageFn onMessage) {
    auto closure = std::make_shared<SubscriptionClosure>();
    closure->fn = std::move(onMessage);
    natsSubscription* sub = nullptr;
    natsStatus s = natsConnection_Subscribe(&sub, m->conn, subject.c_str(), &natsMessageThunk, closure.get());
    if (s != NATS_OK) return makeError(ErrorCode::IoError, "NATS subscribe {}: {}", subject, natsStatus_GetText(s));
    // The on-complete callback keeps the closure alive until nats.c is done with the handler.
    auto* ref = new std::shared_ptr<SubscriptionClosure>(closure);
    s = natsSubscription_SetOnCompleteCB(sub, &natsCompleteThunk, ref);
    if (s != NATS_OK) {
        delete ref;
        natsSubscription_Unsubscribe(sub);
        natsSubscription_Destroy(sub);
        return makeError(ErrorCode::IoError, "NATS subscribe {}: {}", subject, natsStatus_GetText(s));
    }
    std::lock_guard lock(m->subMutex);
    const u64 id = m->nextSub++;
    m->subs.emplace(id, Impl::Sub{sub, std::move(closure)});
    return id;
}

void NatsBus::unsubscribe(u64 subscriptionId) {
    Impl::Sub sub;
    {
        std::lock_guard lock(m->subMutex);
        auto it = m->subs.find(subscriptionId);
        if (it == m->subs.end()) return;
        sub = std::move(it->second);
        m->subs.erase(it);
    }
    // Returns only once the handler cannot run any more (unless called from inside it).
    closeSubscription(sub.sub, sub.closure);
}

Result<void> NatsBus::publish(std::string subject, std::vector<u8> payload, BusHeaders headers) {
    natsMsg* msg = nullptr;
    natsStatus s = natsMsg_Create(&msg, subject.c_str(), nullptr, reinterpret_cast<const char*>(payload.data()),
                                  static_cast<int>(payload.size()));
    for (const auto& [k, v] : headers)
        if (s == NATS_OK) s = natsMsgHeader_Set(msg, k.c_str(), v.c_str());
    if (s == NATS_OK) s = natsConnection_PublishMsg(m->conn, msg);
    natsMsg_Destroy(msg);
    if (s != NATS_OK) return makeError(ErrorCode::IoError, "NATS publish {}: {}", subject, natsStatus_GetText(s));
    return {};
}

} // namespace helios::server
