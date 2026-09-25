#include "helios/server/orch_protocol.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

#include <yyjson.h>

namespace helios::server::orch {

std::string orchestratorSubject(std::string_view shard, std::string_view method) {
    return "rpc." + std::string(shard) + ".orch." + std::string(method);
}
std::string sealTicketsSubject(std::string_view shard) {
    return "rpc." + std::string(shard) + ".session.SealReconnectTickets";
}
std::string gatewayControlSubject(std::string_view shard, std::string_view verb) {
    return "ctl." + std::string(shard) + ".gateway.all." + std::string(verb);
}

namespace {

// ---------------------------------------------------------------------------------------------
// Writing (yyjson mutable documents; every value string is copied into the document)
// ---------------------------------------------------------------------------------------------

class Writer {
public:
    Writer() : m_doc(yyjson_mut_doc_new(nullptr)) {
        m_root = yyjson_mut_obj(m_doc);
        yyjson_mut_doc_set_root(m_doc, m_root);
    }
    ~Writer() { yyjson_mut_doc_free(m_doc); }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    yyjson_mut_val* root() const noexcept { return m_root; }
    yyjson_mut_doc* doc() const noexcept { return m_doc; }

    void str(yyjson_mut_val* obj, const char* key, std::string_view v) {
        yyjson_mut_obj_add_strncpy(m_doc, obj, key, v.data(), v.size());
    }
    void strOmit(yyjson_mut_val* obj, const char* key, std::string_view v) {
        if (!v.empty()) str(obj, key, v);
    }
    /// 64-bit integers are JSON strings (Go `,string`).
    void u64s(yyjson_mut_val* obj, const char* key, u64 v) { str(obj, key, std::to_string(v)); }
    void u64sOmit(yyjson_mut_val* obj, const char* key, u64 v) {
        if (v != 0) u64s(obj, key, v);
    }
    void num(yyjson_mut_val* obj, const char* key, i64 v) { yyjson_mut_obj_add_sint(m_doc, obj, key, v); }
    void numOmit(yyjson_mut_val* obj, const char* key, i64 v) {
        if (v != 0) num(obj, key, v);
    }
    void real(yyjson_mut_val* obj, const char* key, f64 v) {
        // JSON has no NaN or infinity (Go refuses to encode them): report 0 instead of writing a
        // document the service cannot parse.
        if (!std::isfinite(v)) v = 0.0;
        // Integral values are written as integers, as Go does ("0", not "0.0"). The range check
        // keeps the i64 conversion defined.
        if (std::fabs(v) < 9.0e15 && v == std::trunc(v)) num(obj, key, static_cast<i64>(v));
        else yyjson_mut_obj_add_real(m_doc, obj, key, v);
    }
    yyjson_mut_val* arr(yyjson_mut_val* obj, const char* key) { return yyjson_mut_obj_add_arr(m_doc, obj, key); }
    yyjson_mut_val* obj(yyjson_mut_val* parent, const char* key) { return yyjson_mut_obj_add_obj(m_doc, parent, key); }
    yyjson_mut_val* arrObj(yyjson_mut_val* array) { return yyjson_mut_arr_add_obj(m_doc, array); }
    void arrStr(yyjson_mut_val* array, std::string_view v) { yyjson_mut_arr_add_strncpy(m_doc, array, v.data(), v.size()); }

    std::vector<u8> bytes() const {
        usize len = 0;
        char* text = yyjson_mut_write(m_doc, 0, &len);
        std::vector<u8> out;
        if (text) {
            out.assign(reinterpret_cast<const u8*>(text), reinterpret_cast<const u8*>(text) + len);
            std::free(text);
        }
        return out;
    }

private:
    yyjson_mut_doc* m_doc;
    yyjson_mut_val* m_root;
};

void writeAssignments(Writer& w, yyjson_mut_val* obj, const std::vector<Assignment>& list) {
    yyjson_mut_val* a = w.arr(obj, "assignments");
    for (const Assignment& x : list) {
        yyjson_mut_val* o = w.arrObj(a);
        w.u64s(o, "zoneId", x.zoneId);
        w.str(o, "zoneName", x.zoneName);
        w.u64s(o, "leaseGen", x.leaseGen);
    }
}

// ---------------------------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------------------------

class Doc {
public:
    explicit Doc(std::span<const u8> json)
        : m_doc(yyjson_read(reinterpret_cast<const char*>(json.data()), json.size(), 0)) {}
    ~Doc() { yyjson_doc_free(m_doc); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
    yyjson_val* root() const noexcept {
        yyjson_val* r = m_doc ? yyjson_doc_get_root(m_doc) : nullptr;
        return yyjson_is_obj(r) ? r : nullptr;
    }

private:
    yyjson_doc* m_doc;
};

/// Reads a 64-bit unsigned value stored as a JSON string (Go `,string`) or a number. Missing
/// fields read as 0 (Go leaves them zero). Sets `bad` for any other type.
u64 getU64(yyjson_val* obj, const char* key, bool& bad) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (!v || yyjson_is_null(v)) return 0;
    if (yyjson_is_uint(v)) return yyjson_get_uint(v);
    if (yyjson_is_sint(v)) {
        const i64 s = yyjson_get_sint(v);
        if (s < 0) bad = true;
        return static_cast<u64>(s);
    }
    if (yyjson_is_str(v)) {
        const char* s = yyjson_get_str(v);
        const usize n = yyjson_get_len(v);
        u64 out = 0;
        const auto [p, ec] = std::from_chars(s, s + n, out);
        if (ec != std::errc{} || p != s + n) bad = true;
        return out;
    }
    bad = true;
    return 0;
}

i64 getI64(yyjson_val* obj, const char* key, bool& bad) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (!v || yyjson_is_null(v)) return 0;
    if (yyjson_is_int(v)) return yyjson_get_sint(v);
    if (yyjson_is_str(v)) {
        const char* s = yyjson_get_str(v);
        const usize n = yyjson_get_len(v);
        i64 out = 0;
        const auto [p, ec] = std::from_chars(s, s + n, out);
        if (ec != std::errc{} || p != s + n) bad = true;
        return out;
    }
    if (yyjson_is_real(v)) {
        // Only integral values that fit (the conversion is undefined otherwise).
        const f64 d = yyjson_get_real(v);
        if (std::isfinite(d) && d == std::trunc(d) && d >= -9.2e18 && d <= 9.2e18) return static_cast<i64>(d);
    }
    bad = true;
    return 0;
}

f64 getF64(yyjson_val* obj, const char* key, bool& bad) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (!v || yyjson_is_null(v)) return 0.0;
    if (yyjson_is_num(v)) {
        const f64 d = yyjson_get_num(v);
        if (std::isfinite(d)) return d;
    }
    bad = true;
    return 0.0;
}

std::string getStr(yyjson_val* obj, const char* key, bool& bad) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (!v || yyjson_is_null(v)) return {};
    if (!yyjson_is_str(v)) {
        bad = true;
        return {};
    }
    return std::string(yyjson_get_str(v), yyjson_get_len(v));
}

template <class F>
void forEach(yyjson_val* obj, const char* key, bool& bad, F fn) {
    yyjson_val* a = yyjson_obj_get(obj, key);
    if (!a || yyjson_is_null(a)) return;
    if (!yyjson_is_arr(a)) {
        bad = true;
        return;
    }
    usize idx = 0;
    usize max = 0;
    yyjson_val* item = nullptr;
    yyjson_arr_foreach(a, idx, max, item) { fn(item); }
}

std::vector<u64> getU64List(yyjson_val* obj, const char* key, bool& bad) {
    std::vector<u64> out;
    forEach(obj, key, bad, [&](yyjson_val* item) {
        u64 v = 0;
        if (yyjson_is_str(item)) {
            const char* s = yyjson_get_str(item);
            const usize n = yyjson_get_len(item);
            const auto [p, ec] = std::from_chars(s, s + n, v);
            if (ec != std::errc{} || p != s + n) bad = true;
        } else if (yyjson_is_uint(item)) {
            v = yyjson_get_uint(item);
        } else {
            bad = true;
        }
        out.push_back(v);
    });
    return out;
}

std::vector<Assignment> getAssignments(yyjson_val* obj, bool& bad) {
    std::vector<Assignment> out;
    forEach(obj, "assignments", bad, [&](yyjson_val* item) {
        if (!yyjson_is_obj(item)) {
            bad = true;
            return;
        }
        Assignment a;
        a.zoneId = getU64(item, "zoneId", bad);
        a.zoneName = getStr(item, "zoneName", bad);
        a.leaseGen = getU64(item, "leaseGen", bad);
        out.push_back(std::move(a));
    });
    return out;
}

Error bad(std::string_view what) { return Error{ErrorCode::ParseError, "malformed " + std::string(what) + " JSON"}; }

template <class T, class F>
Result<T> decodeWith(std::span<const u8> json, std::string_view what, F fill) {
    Doc doc(json);
    yyjson_val* root = doc.root();
    if (!root) return bad(what);
    bool failed = false;
    T out{};
    fill(root, out, failed);
    if (failed) return bad(what);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Encoders
// ---------------------------------------------------------------------------------------------

std::vector<u8> encode(const ProcessInfo& m) {
    Writer w;
    auto* r = w.root();
    w.str(r, "name", m.name);
    w.str(r, "kind", m.kind);
    w.strOmit(r, "address", m.address);
    w.strOmit(r, "host", m.host);
    w.numOmit(r, "pid", m.pid);
    w.strOmit(r, "version", m.version);
    if (!m.zones.empty()) {
        auto* a = w.arr(r, "zones");
        for (const std::string& z : m.zones) w.arrStr(a, z);
    }
    w.numOmit(r, "keyId", m.keyId);
    w.numOmit(r, "capacity", m.capacity);
    return w.bytes();
}

std::vector<u8> encode(const RegisterResult& m) {
    Writer w;
    auto* r = w.root();
    w.u64s(r, "processId", m.processId);
    w.u64s(r, "epoch", m.epoch);
    w.num(r, "leaseTtlMs", m.leaseTtlMs);
    w.num(r, "heartbeatIntervalMs", m.heartbeatIntervalMs);
    writeAssignments(w, r, m.assignments);
    w.num(r, "idShard", m.idShard);
    auto* a = w.arr(r, "idBlocks");
    for (u64 b : m.idBlocks) w.arrStr(a, std::to_string(b));
    return w.bytes();
}

std::vector<u8> encode(const HeartbeatRequest& m) {
    Writer w;
    auto* r = w.root();
    w.u64s(r, "processId", m.processId);
    w.u64s(r, "epoch", m.epoch);
    auto* load = w.obj(r, "load");
    w.num(load, "players", m.load.players);
    w.num(load, "freeSlots", m.load.freeSlots);
    w.real(load, "tickP99Ms", m.load.tickP99Ms);
    return w.bytes();
}

std::vector<u8> encode(const HeartbeatResult& m) {
    Writer w;
    auto* r = w.root();
    w.str(r, "leaseExpires", m.leaseExpires);
    writeAssignments(w, r, m.assignments);
    w.str(r, "mode", m.mode);
    return w.bytes();
}

std::vector<u8> encode(const AllocateIdBlocksRequest& m) {
    Writer w;
    auto* r = w.root();
    w.u64s(r, "processId", m.processId);
    w.u64s(r, "epoch", m.epoch);
    w.num(r, "n", m.n);
    return w.bytes();
}

std::vector<u8> encode(const AllocateIdBlocksResponse& m) {
    Writer w;
    auto* r = w.root();
    w.num(r, "idShard", m.idShard);
    auto* a = w.arr(r, "prefixes");
    for (u64 p : m.prefixes) w.arrStr(a, std::to_string(p));
    return w.bytes();
}

std::vector<u8> encode(const DeregisterRequest& m) {
    Writer w;
    w.u64s(w.root(), "processId", m.processId);
    w.u64s(w.root(), "epoch", m.epoch);
    return w.bytes();
}

std::vector<u8> encode(const ResolveZoneRequest& m) {
    Writer w;
    w.u64sOmit(w.root(), "zoneId", m.zoneId);
    w.strOmit(w.root(), "zoneName", m.zoneName);
    return w.bytes();
}

std::vector<u8> encode(const Route& m) {
    Writer w;
    auto* r = w.root();
    w.u64s(r, "zoneId", m.zoneId);
    w.str(r, "zoneName", m.zoneName);
    w.u64s(r, "leaseGen", m.leaseGen);
    w.u64s(r, "processId", m.processId);
    w.str(r, "process", m.process);
    w.u64s(r, "epoch", m.epoch);
    w.str(r, "address", m.address);
    return w.bytes();
}

std::vector<u8> encode(const SealRequest& m) {
    Writer w;
    auto* a = w.arr(w.root(), "sessions");
    for (const SealItem& s : m.sessions) {
        auto* o = w.arrObj(a);
        w.u64s(o, "sessionId", s.sessionId);
        w.u64sOmit(o, "zoneId", s.zoneId);
        w.u64sOmit(o, "epoch", s.epoch);
        w.strOmit(o, "ticket", s.ticket);
    }
    return w.bytes();
}

std::vector<u8> encode(const SealResponse& m) {
    Writer w;
    auto* t = w.arr(w.root(), "tickets");
    for (const SealedTicket& s : m.tickets) {
        auto* o = w.arrObj(t);
        w.u64s(o, "sessionId", s.sessionId);
        w.str(o, "ticket", s.ticket);
        w.str(o, "expiresAt", s.expiresAt);
    }
    auto* miss = w.arr(w.root(), "missing");
    for (u64 id : m.missing) w.arrStr(miss, std::to_string(id));
    return w.bytes();
}

std::vector<u8> encode(const ControlMessage& m) {
    Writer w;
    w.u64s(w.root(), "sessionId", m.sessionId);
    w.u64sOmit(w.root(), "epoch", m.epoch);
    w.strOmit(w.root(), "reason", m.reason);
    return w.bytes();
}

// ---------------------------------------------------------------------------------------------
// Decoders
// ---------------------------------------------------------------------------------------------

Result<ProcessInfo> decodeProcessInfo(std::span<const u8> json) {
    return decodeWith<ProcessInfo>(json, "ProcessInfo", [](yyjson_val* r, ProcessInfo& m, bool& b) {
        m.name = getStr(r, "name", b);
        m.kind = getStr(r, "kind", b);
        m.address = getStr(r, "address", b);
        m.host = getStr(r, "host", b);
        m.pid = getI64(r, "pid", b);
        m.version = getStr(r, "version", b);
        forEach(r, "zones", b, [&](yyjson_val* z) {
            if (yyjson_is_str(z)) m.zones.emplace_back(yyjson_get_str(z), yyjson_get_len(z));
            else b = true;
        });
        m.keyId = static_cast<u32>(getI64(r, "keyId", b));
        m.capacity = getI64(r, "capacity", b);
    });
}

Result<RegisterResult> decodeRegisterResult(std::span<const u8> json) {
    return decodeWith<RegisterResult>(json, "RegisterResult", [](yyjson_val* r, RegisterResult& m, bool& b) {
        m.processId = getU64(r, "processId", b);
        m.epoch = getU64(r, "epoch", b);
        m.leaseTtlMs = getI64(r, "leaseTtlMs", b);
        m.heartbeatIntervalMs = getI64(r, "heartbeatIntervalMs", b);
        m.assignments = getAssignments(r, b);
        m.idShard = static_cast<u32>(getI64(r, "idShard", b));
        m.idBlocks = getU64List(r, "idBlocks", b);
        if (m.processId == 0) b = true;
    });
}

Result<HeartbeatRequest> decodeHeartbeatRequest(std::span<const u8> json) {
    return decodeWith<HeartbeatRequest>(json, "HeartbeatRequest", [](yyjson_val* r, HeartbeatRequest& m, bool& b) {
        m.processId = getU64(r, "processId", b);
        m.epoch = getU64(r, "epoch", b);
        if (yyjson_val* load = yyjson_obj_get(r, "load"); yyjson_is_obj(load)) {
            m.load.players = getI64(load, "players", b);
            m.load.freeSlots = getI64(load, "freeSlots", b);
            m.load.tickP99Ms = getF64(load, "tickP99Ms", b);
        }
    });
}

Result<HeartbeatResult> decodeHeartbeatResult(std::span<const u8> json) {
    return decodeWith<HeartbeatResult>(json, "HeartbeatResult", [](yyjson_val* r, HeartbeatResult& m, bool& b) {
        m.leaseExpires = getStr(r, "leaseExpires", b);
        m.assignments = getAssignments(r, b);
        m.mode = getStr(r, "mode", b);
    });
}

Result<AllocateIdBlocksRequest> decodeAllocateIdBlocksRequest(std::span<const u8> json) {
    return decodeWith<AllocateIdBlocksRequest>(json, "AllocateIdBlocksRequest",
                                               [](yyjson_val* r, AllocateIdBlocksRequest& m, bool& b) {
                                                   m.processId = getU64(r, "processId", b);
                                                   m.epoch = getU64(r, "epoch", b);
                                                   m.n = static_cast<u32>(getI64(r, "n", b));
                                               });
}

Result<AllocateIdBlocksResponse> decodeAllocateIdBlocksResponse(std::span<const u8> json) {
    return decodeWith<AllocateIdBlocksResponse>(json, "AllocateIdBlocksResponse",
                                                [](yyjson_val* r, AllocateIdBlocksResponse& m, bool& b) {
                                                    m.idShard = static_cast<u32>(getI64(r, "idShard", b));
                                                    m.prefixes = getU64List(r, "prefixes", b);
                                                });
}

Result<DeregisterRequest> decodeDeregisterRequest(std::span<const u8> json) {
    return decodeWith<DeregisterRequest>(json, "DeregisterRequest", [](yyjson_val* r, DeregisterRequest& m, bool& b) {
        m.processId = getU64(r, "processId", b);
        m.epoch = getU64(r, "epoch", b);
    });
}

Result<ResolveZoneRequest> decodeResolveZoneRequest(std::span<const u8> json) {
    return decodeWith<ResolveZoneRequest>(json, "ResolveZoneRequest", [](yyjson_val* r, ResolveZoneRequest& m, bool& b) {
        m.zoneId = getU64(r, "zoneId", b);
        m.zoneName = getStr(r, "zoneName", b);
    });
}

Result<Route> decodeRoute(std::span<const u8> json) {
    return decodeWith<Route>(json, "Route", [](yyjson_val* r, Route& m, bool& b) {
        m.zoneId = getU64(r, "zoneId", b);
        m.zoneName = getStr(r, "zoneName", b);
        m.leaseGen = getU64(r, "leaseGen", b);
        m.processId = getU64(r, "processId", b);
        m.process = getStr(r, "process", b);
        m.epoch = getU64(r, "epoch", b);
        m.address = getStr(r, "address", b);
    });
}

Result<SealRequest> decodeSealRequest(std::span<const u8> json) {
    return decodeWith<SealRequest>(json, "SealRequest", [](yyjson_val* r, SealRequest& m, bool& b) {
        forEach(r, "sessions", b, [&](yyjson_val* item) {
            if (!yyjson_is_obj(item)) {
                b = true;
                return;
            }
            SealItem s;
            s.sessionId = getU64(item, "sessionId", b);
            s.zoneId = getU64(item, "zoneId", b);
            s.epoch = getU64(item, "epoch", b);
            s.ticket = getStr(item, "ticket", b);
            m.sessions.push_back(std::move(s));
        });
    });
}

Result<SealResponse> decodeSealResponse(std::span<const u8> json) {
    return decodeWith<SealResponse>(json, "SealResponse", [](yyjson_val* r, SealResponse& m, bool& b) {
        forEach(r, "tickets", b, [&](yyjson_val* item) {
            if (!yyjson_is_obj(item)) {
                b = true;
                return;
            }
            SealedTicket t;
            t.sessionId = getU64(item, "sessionId", b);
            t.ticket = getStr(item, "ticket", b);
            t.expiresAt = getStr(item, "expiresAt", b);
            m.tickets.push_back(std::move(t));
        });
        m.missing = getU64List(r, "missing", b);
    });
}

Result<ControlMessage> decodeControlMessage(std::span<const u8> json) {
    return decodeWith<ControlMessage>(json, "ControlMessage", [](yyjson_val* r, ControlMessage& m, bool& b) {
        m.sessionId = getU64(r, "sessionId", b);
        m.epoch = getU64(r, "epoch", b);
        m.reason = getStr(r, "reason", b);
    });
}

// ---------------------------------------------------------------------------------------------
// RFC 3339
// ---------------------------------------------------------------------------------------------

namespace {
// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil).
i64 daysFromCivil(i64 y, unsigned m, unsigned d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const i64 era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<i64>(doe) - 719468;
}

void civilFromDays(i64 z, i64& y, unsigned& m, unsigned& d) noexcept {
    z += 719468;
    const i64 era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<i64>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2 ? 1 : 0;
}

bool digits(std::string_view s, usize pos, usize n, i64& out) noexcept {
    if (pos + n > s.size()) return false;
    out = 0;
    for (usize i = 0; i < n; ++i) {
        const char c = s[pos + i];
        if (c < '0' || c > '9') return false;
        out = out * 10 + (c - '0');
    }
    return true;
}
} // namespace

Result<i64> parseRfc3339UnixMs(std::string_view s) {
    const Error err{ErrorCode::ParseError, "bad RFC 3339 time"};
    i64 year = 0, mon = 0, day = 0, hh = 0, mm = 0, ss = 0;
    if (!digits(s, 0, 4, year) || s.size() < 19 || s[4] != '-' || !digits(s, 5, 2, mon) || s[7] != '-' ||
        !digits(s, 8, 2, day) || (s[10] != 'T' && s[10] != 't') || !digits(s, 11, 2, hh) || s[13] != ':' ||
        !digits(s, 14, 2, mm) || s[16] != ':' || !digits(s, 17, 2, ss))
        return err;
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60) return err;
    usize pos = 19;
    i64 millis = 0;
    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        usize n = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            if (n < 3) millis = millis * 10 + (s[pos] - '0');
            ++n;
            ++pos;
        }
        if (n == 0) return err;
        for (usize i = n; i < 3; ++i) millis *= 10;
    }
    i64 offsetMin = 0;
    if (pos < s.size() && (s[pos] == 'Z' || s[pos] == 'z')) {
        ++pos;
    } else if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
        const i64 sign = s[pos] == '-' ? -1 : 1;
        i64 oh = 0, om = 0;
        if (!digits(s, pos + 1, 2, oh) || pos + 3 >= s.size() || s[pos + 3] != ':' || !digits(s, pos + 4, 2, om)) return err;
        offsetMin = sign * (oh * 60 + om);
        pos += 6;
    } else {
        return err;
    }
    if (pos != s.size()) return err;
    const i64 days = daysFromCivil(year, static_cast<unsigned>(mon), static_cast<unsigned>(day));
    const i64 secs = days * 86400 + hh * 3600 + mm * 60 + ss - offsetMin * 60;
    return secs * 1000 + millis;
}

std::string formatRfc3339UnixMs(i64 unixMs) {
    i64 secs = unixMs >= 0 ? unixMs / 1000 : -((-unixMs + 999) / 1000);
    const i64 millis = unixMs - secs * 1000;
    i64 days = secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
    const i64 rem = secs - days * 86400;
    i64 y = 0;
    unsigned m = 0, d = 0;
    civilFromDays(days, y, m, d);
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", y, m, d, rem / 3600, (rem / 60) % 60, rem % 60, millis);
}

} // namespace helios::server::orch
