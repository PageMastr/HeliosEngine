#include "workload.h"

#include <algorithm>
#include <format>
#include <string>

#include "helios/reflect/json.h"
#include "helios/reflect/path.h"
#include "sample/ship.gen.h"

namespace helios::tf::test {

namespace {

using sample::ship::ShipHullDef;

std::string f32Json(f32 v) {
    char buf[40];
    const usize n = refl::formatJsonF32(v, buf);
    return std::string(buf, n);
}

const ShipHullDef& hull(const Framework& fw, const DocId& doc) {
    return *static_cast<const ShipHullDef*>(fw.documents().find(doc)->object());
}

constexpr const char* kSizes[] = {"\"Small\"", "\"Medium\"", "\"Large\"", "\"Capital\""};
constexpr const char* kFactions[] = {"\"Neutral\"", "\"Pilots\"", "\"Syndicate\"", "\"Drones\""};
constexpr const char* kRates[] = {"handling/pitchRate", "handling/yawRate", "handling/rollRate"};

} // namespace

Result<bool> Workload::step(Framework& fw, const DocId& doc) {
    ++m_attempts;
    const u32 roll = below(100);
    if (m_options.undoRedo && roll < 10) {
        if (!fw.canUndo()) return false;
        HELIOS_TRY(fw.undo(Origin::Cli));
        return true;
    }
    if (m_options.undoRedo && roll < 15) {
        if (!fw.canRedo()) return false;
        HELIOS_TRY(fw.redo(Origin::Cli));
        return true;
    }
    return doEdit(fw, doc, below(21));
}

Result<bool> Workload::doEdit(Framework& fw, const DocId& doc, u32 kind) {
    const u64 n = ++m_counter;
    if (kind == 19 && !m_options.invalid) kind = 0;
    if (kind == 20 && !m_options.mergeKeys) kind = 1;

    if (kind == 20) {
        // A continuous gesture: several commits with one mergeKey (one undo step).
        const std::string key = std::format("gesture-{}", n);
        const u32 count = 2 + below(4);
        bool any = false;
        for (u32 i = 0; i < count; ++i) {
            auto b = fw.begin(Origin::Ui, "Drag yaw rate");
            b->setMergeKey(key);
            HELIOS_TRY(b->set(doc, "handling/yawRate", f32Json(static_cast<f32>(static_cast<i32>(below(1601)) - 800) / 8.0f)));
            HELIOS_TRY_ASSIGN(const TxId id, b->commit());
            any = any || !id.isNull();
        }
        return any;
    }

    const u32 opCount = kind == 18 ? 2 + below(3) : 1;
    auto b = fw.begin(Origin::Cli, std::format("step {}", n));
    for (u32 op = 0; op < opCount; ++op) {
        const u32 k = kind == 18 ? below(18) : kind;
        const ShipHullDef& h = hull(fw, doc);
        Result<void> r;
        switch (k) {
        case 0: r = b->set(doc, "mass", f32Json(100.0f + static_cast<f32>(below(399601)) / 4.0f)); break;
        case 1: {
            // Draw in statement order (argument evaluation order differs between compilers).
            const char* rate = kRates[below(3)];
            const std::string value = f32Json(static_cast<f32>(static_cast<i32>(below(1601)) - 800) / 8.0f);
            r = b->set(doc, rate, value);
            break;
        }
        case 2: r = b->set(doc, "size", kSizes[below(4)]); break;
        case 3: r = b->set(doc, "faction", kFactions[below(4)]); break;
        case 4: r = b->set(doc, "name", std::format("\"ship.test.{}\"", below(1000))); break;
        case 5: {
            if (h.thrusters.size() >= 60) break;
            // One draw per statement: the evaluation order of function arguments differs between
            // compilers, and the golden hash must not.
            const i32 dx = static_cast<i32>(below(3)) - 1;
            const i32 dy = static_cast<i32>(below(3)) - 1;
            const i32 dz = static_cast<i32>(below(3)) - 1;
            const u32 force = 16 * below(60000);
            const std::string elem =
                std::format(R"({{"bone": "t{}", "dir": [{}, {}, {}], "maxForce": {}}})", ++m_counter, dx, dy, dz, force);
            r = b->insert(doc, "thrusters", below(static_cast<u32>(h.thrusters.size()) + 1), elem);
            break;
        }
        case 6:
            if (h.thrusters.empty()) break;
            r = b->remove(doc, std::format("thrusters[#{}]", refl::keyedKeyText(h.thrusters.keyAt(below(static_cast<u32>(h.thrusters.size()))))));
            break;
        case 7: {
            if (h.thrusters.size() < 2) break;
            const u32 count = static_cast<u32>(h.thrusters.size());
            const std::string key = refl::keyedKeyText(h.thrusters.keyAt(below(count)));
            r = b->move(doc, std::format("thrusters[#{}]", key), below(count));
            break;
        }
        case 8: {
            if (h.thrusters.empty()) break;
            const std::string key = refl::keyedKeyText(h.thrusters.keyAt(below(static_cast<u32>(h.thrusters.size()))));
            switch (below(3)) {
            case 0: r = b->set(doc, std::format("thrusters[#{}]/maxForce", key), std::to_string(16 * below(60000))); break;
            case 1: {
                const i32 dx = static_cast<i32>(below(5)) - 2;
                const i32 dy = static_cast<i32>(below(5)) - 2;
                const i32 dz = static_cast<i32>(below(5)) - 2;
                r = b->set(doc, std::format("thrusters[#{}]/dir", key), std::format("[{}, {}, {}]", dx, dy, dz));
                break;
            }
            default: r = b->set(doc, std::format("thrusters[#{}]/bone", key), std::format("\"bone{}\"", below(50))); break;
            }
            break;
        }
        case 9: {
            if (h.hardpoints.size() >= 24) break;
            const std::string_view size = kSizes[below(4)];
            const u32 ox = below(8);
            const u32 oz = below(8);
            const std::string elem =
                std::format(R"({{"slot": "hp{}", "size": {}, "offset": [{}, 0.5, -{}]}})", ++m_counter, size, ox, oz);
            r = b->insert(doc, "hardpoints", below(static_cast<u32>(h.hardpoints.size()) + 1), elem);
            break;
        }
        case 10:
            if (h.hardpoints.empty()) break;
            r = b->remove(doc, std::format("hardpoints[#{}]", h.hardpoints[below(static_cast<u32>(h.hardpoints.size()))].slot.view()));
            break;
        case 11: {
            if (h.hardpoints.empty()) break;
            const std::string slot(h.hardpoints[below(static_cast<u32>(h.hardpoints.size()))].slot.view());
            if (below(2) == 0) {
                r = b->set(doc, std::format("hardpoints[#{}]/size", slot), kSizes[below(4)]);
            } else {
                const std::string ox = f32Json(static_cast<f32>(below(64)) / 16.0f);
                const u32 oy = below(4);
                const u32 oz = below(9);
                r = b->set(doc, std::format("hardpoints[#{}]/offset", slot), std::format("[{}, {}, {}]", ox, oy, oz));
            }
            break;
        }
        case 12: {
            const u32 hint = below(12);
            const std::string value = f32Json(static_cast<f32>(below(4096)) / 64.0f);
            r = b->set(doc, std::format("aiHints[k{}]", hint), value);
            break;
        }
        case 13: {
            if (h.aiHints.empty()) break;
            // std::map<Name, ...> iterates in interning order, which differs between processes;
            // pick by text so the choice is the same everywhere.
            std::string_view smallest = h.aiHints.begin()->first.view();
            for (const auto& [key, value] : h.aiHints) smallest = std::min(smallest, key.view());
            r = b->remove(doc, std::format("aiHints[{}]", smallest));
            break;
        }
        case 14:
            r = below(3) == 0 ? b->set(doc, "lootTable", "null") : b->set(doc, "lootTable", std::to_string(1000 + below(9000)));
            break;
        case 15: {
            const u16 bits = static_cast<u16>(below(16));
            r = b->edit(doc, [bits](void* obj) {
                static_cast<ShipHullDef*>(obj)->capabilities = static_cast<sample::common::Capability>(bits);
            });
            break;
        }
        case 16: {
            const f32 mass = 200.0f + static_cast<f32>(below(4000));
            const bool reverse = below(2) == 0;
            r = b->edit(doc, [mass, reverse](void* obj) {
                auto* s = static_cast<ShipHullDef*>(obj);
                s->mass = mass;
                if (reverse && s->thrusters.size() >= 2) {
                    // A reorder: the diff sets the whole keyed list.
                    refl::KeyedList<sample::ship::ThrusterMount> reversed;
                    for (usize i = s->thrusters.size(); i-- > 0;) reversed.add(s->thrusters.keyAt(i), s->thrusters[i]);
                    s->thrusters = std::move(reversed);
                }
            });
            break;
        }
        case 17:
            r = below(2) == 0 ? b->set(doc, "$comment", std::format("\"note {}\"", below(100)))
                              : b->set(doc, "$name", std::format("\"hull/frigate{}\"", below(4)));
            break;
        default:
            // 19: out of @range(100, 1e9); the pre-commit hook must reject the whole transaction.
            r = b->set(doc, "mass", "50");
            break;
        }
        if (!r) {
            b->abort();
            return r.error();
        }
    }
    auto committed = b->commit();
    if (!committed) {
        if (kind == 19) {
            // A rejected transaction must leave the document untouched (the caller checks hashes).
            ++m_rejected;
            return false;
        }
        return committed.error();
    }
    return !committed->isNull();
}

} // namespace helios::tf::test
