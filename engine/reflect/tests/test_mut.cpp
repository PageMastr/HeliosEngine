// Mut<C> contract: the shape helios-schemac generates for replicated components.

#include <doctest/doctest.h>

#include <tuple>

#include "helios/reflect/codec.h"
#include "helios/reflect/mut.h"

using namespace helios;
using namespace helios::refl;

namespace {
struct Motion {
    Vec3 vel;
    f32 throttle = 0;
    u64 _dirty = 0;
    static constexpr auto kReplicatedFields = std::make_tuple(&Motion::vel, &Motion::throttle);
};
} // namespace

// Hand-written copy of what the generator emits.
template <>
class helios::refl::Mut<Motion> : public helios::refl::detail::MutBase<Motion> {
public:
    using MutBase::MutBase;
    static constexpr u64 kVel = 1ull << 0;
    static constexpr u64 kThrottle = 1ull << 1;
    static constexpr u64 kAll = kVel | kThrottle;
    const Vec3& vel() const noexcept { return m_c->vel; }
    void setVel(const Vec3& v) {
        if (Codec<Vec3>::equals(m_c->vel, v)) return;
        m_c->vel = v;
        markBits(kVel);
    }
    void setThrottle(f32 v) {
        if (Codec<f32>::equals(m_c->throttle, v)) return;
        m_c->throttle = v;
        markBits(kThrottle);
    }
    Motion& raw() noexcept {
        markBits(kAll);
        return *m_c;
    }
};

TEST_CASE("mut: setters mark field bits and the entity summary") {
    Motion m;
    u64 summary = 0;
    Mut<Motion> mut(m, &summary, 1ull << 5);
    mut.setThrottle(0.0f); // unchanged value: nothing marked
    CHECK(dirtyFields(m) == 0);
    CHECK(summary == 0);
    mut.setThrottle(0.5f);
    CHECK(dirtyFields(m) == Mut<Motion>::kThrottle);
    CHECK(summary == (1ull << 5));
    mut.setVel(Vec3(1, 0, 0));
    CHECK(mut.dirtyFields() == Mut<Motion>::kAll);
    clearDirty(m);
    CHECK(dirtyFields(m) == 0);
    mut.raw().vel.y = 2;
    CHECK(dirtyFields(m) == Mut<Motion>::kAll);
    CHECK(std::tuple_size_v<decltype(Motion::kReplicatedFields)> == 2);

    Motion n;
    Mut<Motion> noSummary(n);
    noSummary.setVel(Vec3(0, 1, 0));
    CHECK(dirtyFields(n) == Mut<Motion>::kVel);
}
