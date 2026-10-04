#pragma once
// AssetHandle<T> and AssetStore<T>: loaded asset instances addressed by a core generational Handle
// (02 §6.1), with hot-reload swaps that take effect only at a frame or tick boundary (02 §6.4).
//
// A handle names a loaded instance, not a version of it. Hot reload stages a new version with
// stage(); it becomes what pin() returns after the owner's next commitSwaps(), which the owner calls
// at a frame or tick boundary, so every reader in one frame or tick sees the same version. A version
// stays alive while anyone holds its pin (the "references drain" rule of 02 §6.4); the store drops
// its own reference at the swap. Removing an instance makes its handle stale; pins taken earlier stay
// valid.
//
// Threading: every AssetStore member function is thread-safe (one internal mutex; no callbacks run
// under it). Pins are shared_ptr<const T>: share them freely; the store never mutates a published
// version. Handles are process-local values (core/handle.h): never serialize them.

#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "helios/asset/asset_id.h"
#include "helios/core/handle.h"
#include "helios/core/types.h"

namespace helios::asset {

template <class T>
struct AssetTag {};

/// Handle of a loaded asset instance of type T (core Handle; distinct per T).
template <class T>
using AssetHandle = Handle<AssetTag<T>>;

template <class T>
class AssetStore {
public:
    using Pin = std::shared_ptr<const T>;

    /// Adds a loaded instance of `id`. Returns an invalid handle when `id` is invalid, `value` is null
    /// or `id` is already loaded in this store (find() returns that one).
    AssetHandle<T> insert(AssetId id, Pin value) {
        if (!id.isValid() || !value) return {};
        std::lock_guard lock(m_mutex);
        if (m_byId.count(id) != 0) return {};
        const AssetHandle<T> h = m_pool.create(Slot{id, std::move(value), nullptr});
        m_byId.emplace(id, h);
        return h;
    }

    /// The handle of the loaded instance of `id`, or an invalid handle.
    AssetHandle<T> find(AssetId id) const {
        std::lock_guard lock(m_mutex);
        const auto it = m_byId.find(id);
        return it == m_byId.end() ? AssetHandle<T>{} : it->second;
    }

    /// The current version, or null for a stale or invalid handle. Valid for as long as it is held.
    Pin pin(AssetHandle<T> h) const {
        std::lock_guard lock(m_mutex);
        const Slot* s = m_pool.get(h);
        return s ? s->current : nullptr;
    }

    /// The asset id of a live handle (an invalid id for a stale one).
    AssetId idOf(AssetHandle<T> h) const {
        std::lock_guard lock(m_mutex);
        const Slot* s = m_pool.get(h);
        return s ? s->id : AssetId{};
    }

    /// Stages `next` as the version pin() returns after the next commitSwaps(). A later stage() before
    /// the commit replaces an earlier one, which is released outside the lock. False for a stale
    /// handle or a null `next`.
    bool stage(AssetHandle<T> h, Pin next) {
        if (!next) return false;
        Pin replaced; // released after the lock, like commitSwaps()'s
        {
            std::lock_guard lock(m_mutex);
            Slot* s = m_pool.get(h);
            if (!s) return false;
            if (!s->staged) ++m_stagedCount;
            replaced = std::exchange(s->staged, std::move(next));
        }
        return true;
    }

    /// Publishes every staged version; returns how many swapped. Call at a frame or tick boundary.
    /// The replaced versions are released outside the lock (their destructors may be arbitrary).
    usize commitSwaps() {
        std::vector<Pin> released;
        usize swapped = 0;
        {
            std::lock_guard lock(m_mutex);
            if (m_stagedCount == 0) return 0;
            released.reserve(m_stagedCount);
            m_pool.forEach([&](AssetHandle<T>, Slot& s) {
                if (!s.staged) return;
                released.push_back(std::exchange(s.current, std::move(s.staged)));
                s.staged = nullptr;
                ++swapped;
            });
            m_stagedCount = 0;
        }
        return swapped;
    }

    /// Unloads the instance; its handle becomes stale. False for a stale handle.
    bool erase(AssetHandle<T> h) {
        Slot dropped;
        {
            std::lock_guard lock(m_mutex);
            Slot* s = m_pool.get(h);
            if (!s) return false;
            if (s->staged) --m_stagedCount;
            m_byId.erase(s->id);
            dropped = std::move(*s);
            m_pool.destroy(h);
        }
        return true;
    }

    /// Number of loaded instances.
    u32 size() const {
        std::lock_guard lock(m_mutex);
        return m_pool.size();
    }

private:
    struct Slot {
        AssetId id;
        Pin current;
        Pin staged;
    };

    mutable std::mutex m_mutex;
    HandlePool<Slot, AssetTag<T>> m_pool;
    std::unordered_map<AssetId, AssetHandle<T>> m_byId;
    usize m_stagedCount = 0;
};

} // namespace helios::asset
