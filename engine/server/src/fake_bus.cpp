#include "helios/server/fake_bus.h"

#include <string>

namespace helios::server {

std::string_view BusMessage::header(std::string_view name) const noexcept {
    for (const auto& [k, v] : headers)
        if (k == name) return v;
    return {};
}

std::string_view busStatusName(BusStatus status) noexcept {
    switch (status) {
    case BusStatus::Ok: return "ok";
    case BusStatus::Timeout: return "timeout";
    case BusStatus::NoResponders: return "no_responders";
    case BusStatus::Disconnected: return "disconnected";
    case BusStatus::Error: return "error";
    }
    return "?";
}

bool subjectMatches(std::string_view pattern, std::string_view subject) noexcept {
    for (;;) {
        const usize pd = pattern.find('.');
        const usize sd = subject.find('.');
        const std::string_view pt = pattern.substr(0, pd);
        const std::string_view st = subject.substr(0, sd);
        if (pt == ">") return !subject.empty();
        if (subject.empty() || st.empty()) return false;
        if (pt != "*" && pt != st) return false;
        if (pd == std::string_view::npos || sd == std::string_view::npos) return pd == std::string_view::npos && sd == std::string_view::npos;
        pattern.remove_prefix(pd + 1);
        subject.remove_prefix(sd + 1);
    }
}

FakeBus::FakeBus() = default;
FakeBus::~FakeBus() = default;

void FakeBus::request(std::string subject, std::vector<u8> payload, BusHeaders headers, std::chrono::milliseconds timeout,
                      BusReplyFn onReply) {
    std::lock_guard lock(m_mutex);
    BusMessage msg;
    msg.subject = std::move(subject);
    msg.data = std::move(payload);
    msg.headers = std::move(headers);
    bool hasDeadline = false;
    for (const auto& [k, v] : msg.headers) hasDeadline |= k == kHeaderDeadlineMs;
    if (!hasDeadline) msg.headers.emplace_back(std::string(kHeaderDeadlineMs), std::to_string(timeout.count()));
    msg.reply = "_INBOX.fake." + std::to_string(m_nextInbox++);
    // Failures are delivered by pump() too, never from inside request().
    if (!m_connected) {
        m_failed.push_back(std::move(onReply));
        return;
    }
    m_pending.emplace(msg.reply, Pending{std::move(onReply), m_nowMs + static_cast<i64>(timeout.count())});
    m_queue.push_back(Queued{std::move(msg)});
}

Result<u64> FakeBus::subscribe(std::string subject, BusMessageFn onMessage) {
    if (subject.empty()) return Error{ErrorCode::InvalidArgument, "empty subject"};
    std::lock_guard lock(m_mutex);
    const u64 id = m_nextSub++;
    m_subs.emplace(id, Sub{std::move(subject), std::move(onMessage)});
    return id;
}

void FakeBus::unsubscribe(u64 subscriptionId) {
    std::lock_guard lock(m_mutex);
    m_subs.erase(subscriptionId);
}

Result<void> FakeBus::publish(std::string subject, std::vector<u8> payload, BusHeaders headers) {
    std::lock_guard lock(m_mutex);
    if (!m_connected) return Error{ErrorCode::IoError, "bus disconnected"};
    m_published.push_back(subject);
    BusMessage msg;
    msg.subject = std::move(subject);
    msg.data = std::move(payload);
    msg.headers = std::move(headers);
    m_queue.push_back(Queued{std::move(msg)});
    return {};
}

bool FakeBus::isConnected() const {
    std::lock_guard lock(m_mutex);
    return m_connected;
}

void FakeBus::setConnected(bool connected) {
    std::lock_guard lock(m_mutex);
    m_connected = connected;
    if (!connected) {
        // In-flight traffic is lost; requesters see Disconnected at the next pump.
        for (auto& [inbox, p] : m_pending) m_failed.push_back(std::move(p.onReply));
        m_pending.clear();
        m_queue.clear();
    }
}

i64 FakeBus::nowMs() const {
    std::lock_guard lock(m_mutex);
    return m_nowMs;
}

usize FakeBus::publishedCount(std::string_view pattern) const {
    std::lock_guard lock(m_mutex);
    usize n = 0;
    for (const std::string& s : m_published) n += subjectMatches(pattern, s) ? 1 : 0;
    return n;
}

usize FakeBus::pendingRequests() const {
    std::lock_guard lock(m_mutex);
    return m_pending.size();
}

u32 FakeBus::pump(i64 nowMs, u32 maxRounds) {
    u32 calls = 0;
    struct Delivery {
        BusMessageFn fn;
        BusMessage msg;
    };
    struct ReplyDelivery {
        BusReplyFn fn;
        BusReply reply;
    };
    auto run = [&calls](std::vector<Delivery>& deliveries, std::vector<ReplyDelivery>& replies) {
        for (Delivery& d : deliveries) {
            d.fn(d.msg);
            ++calls;
        }
        for (ReplyDelivery& r : replies) {
            r.fn(std::move(r.reply));
            ++calls;
        }
    };
    for (u32 round = 0; round < maxRounds; ++round) {
        std::vector<Delivery> deliveries;
        std::vector<ReplyDelivery> replies;
        {
            std::lock_guard lock(m_mutex);
            if (nowMs > m_nowMs) m_nowMs = nowMs;
            std::deque<Queued> queue;
            queue.swap(m_queue);
            for (Queued& q : queue) {
                auto pit = m_pending.find(q.message.subject);
                if (pit != m_pending.end()) { // a reply to a request
                    BusReply r;
                    r.status = BusStatus::Ok;
                    r.message = std::move(q.message);
                    replies.push_back(ReplyDelivery{std::move(pit->second.onReply), std::move(r)});
                    m_pending.erase(pit);
                    continue;
                }
                bool matched = false;
                for (auto& [id, sub] : m_subs) {
                    if (!subjectMatches(sub.pattern, q.message.subject)) continue;
                    matched = true;
                    deliveries.push_back(Delivery{sub.fn, q.message});
                }
                if (!matched && !q.message.reply.empty()) {
                    auto req = m_pending.find(q.message.reply);
                    if (req != m_pending.end()) {
                        BusReply r;
                        r.status = BusStatus::NoResponders;
                        r.transportError = "no responders for " + q.message.subject;
                        replies.push_back(ReplyDelivery{std::move(req->second.onReply), std::move(r)});
                        m_pending.erase(req);
                    }
                }
            }
        }
        if (deliveries.empty() && replies.empty()) break;
        run(deliveries, replies);
    }
    // Failures and timeouts, after every queued message had its chance to be answered.
    std::vector<Delivery> none;
    std::vector<ReplyDelivery> failures;
    {
        std::lock_guard lock(m_mutex);
        for (BusReplyFn& fn : m_failed) {
            BusReply r;
            r.status = BusStatus::Disconnected;
            r.transportError = "bus disconnected";
            failures.push_back(ReplyDelivery{std::move(fn), std::move(r)});
        }
        m_failed.clear();
        for (auto it = m_pending.begin(); it != m_pending.end();) {
            if (it->second.deadlineMs <= m_nowMs) {
                BusReply r;
                r.status = BusStatus::Timeout;
                r.transportError = "request timed out";
                failures.push_back(ReplyDelivery{std::move(it->second.onReply), std::move(r)});
                it = m_pending.erase(it);
            } else {
                ++it;
            }
        }
    }
    run(none, failures);
    return calls;
}

} // namespace helios::server
