// AssetHandle<T> over core handles and AssetStore<T>'s frame-boundary swaps (02 §6.1, §6.4).

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "helios/asset/asset_handle.h"

namespace {

using namespace helios;
using namespace helios::asset;

struct Mesh {
    int version = 0;
};

static_assert(!std::is_convertible_v<AssetHandle<Mesh>, AssetHandle<std::string>>,
              "handles of different asset types are distinct types");

TEST_CASE("asset store: insert, find, pin and erase by handle") {
    AssetStore<Mesh> store;
    const AssetId id{42};
    const AssetHandle<Mesh> h = store.insert(id, std::make_shared<Mesh>(Mesh{1}));
    REQUIRE(h.isValid());
    CHECK(store.find(id) == h);
    CHECK(store.idOf(h) == id);
    CHECK(store.pin(h)->version == 1);
    CHECK(store.size() == 1);
    // One instance per id; invalid ids and null values are refused.
    CHECK_FALSE(store.insert(id, std::make_shared<Mesh>(Mesh{2})).isValid());
    CHECK_FALSE(store.insert(AssetId{}, std::make_shared<Mesh>()).isValid());
    CHECK_FALSE(store.insert(AssetId{7}, nullptr).isValid());

    const auto kept = store.pin(h);
    CHECK(store.erase(h));
    CHECK_FALSE(store.erase(h));
    CHECK(store.pin(h) == nullptr); // stale handle
    CHECK_FALSE(store.find(id).isValid());
    CHECK_FALSE(store.stage(h, std::make_shared<Mesh>()));
    CHECK(kept->version == 1); // pins outlive the instance
    // A new instance in the old slot gets a new generation: the old handle stays stale.
    const AssetHandle<Mesh> again = store.insert(id, std::make_shared<Mesh>(Mesh{3}));
    CHECK(again != h);
    CHECK(store.pin(h) == nullptr);
    CHECK(store.pin(again)->version == 3);
}

TEST_CASE("asset store: a staged version becomes visible only at commitSwaps") {
    AssetStore<Mesh> store;
    const AssetHandle<Mesh> a = store.insert(AssetId{1}, std::make_shared<Mesh>(Mesh{1}));
    const AssetHandle<Mesh> b = store.insert(AssetId{2}, std::make_shared<Mesh>(Mesh{1}));
    const auto oldA = store.pin(a);

    CHECK(store.stage(a, std::make_shared<Mesh>(Mesh{2})));
    CHECK(store.stage(a, std::make_shared<Mesh>(Mesh{3}))); // a later stage replaces the earlier one
    CHECK_FALSE(store.stage(a, nullptr));
    CHECK(store.pin(a)->version == 1); // mid-frame readers still see the old version
    CHECK(store.commitSwaps() == 1);
    CHECK(store.pin(a)->version == 3);
    CHECK(store.pin(b)->version == 1);
    CHECK(oldA->version == 1); // the old version lives until its last pin drains
    CHECK(oldA.use_count() == 1);
    CHECK(store.commitSwaps() == 0);

    // Erasing an instance drops its staged version too.
    CHECK(store.stage(b, std::make_shared<Mesh>(Mesh{9})));
    CHECK(store.erase(b));
    CHECK(store.commitSwaps() == 0);
}

TEST_CASE("asset store: concurrent pins while another thread stages and commits") {
    AssetStore<Mesh> store;
    const AssetHandle<Mesh> h = store.insert(AssetId{5}, std::make_shared<Mesh>(Mesh{0}));
    std::thread writer([&] {
        for (int v = 1; v <= 200; ++v) {
            store.stage(h, std::make_shared<Mesh>(Mesh{v}));
            store.commitSwaps();
        }
    });
    int last = 0;
    bool monotonic = true;
    for (int i = 0; i < 2000; ++i) {
        const auto p = store.pin(h);
        monotonic = monotonic && p && p->version >= last;
        if (p) last = p->version;
    }
    writer.join();
    CHECK(monotonic);
    CHECK(store.pin(h)->version == 200);
}

} // namespace
