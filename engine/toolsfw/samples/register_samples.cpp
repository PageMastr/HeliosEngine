#include "helios/toolsfw/samples.h"

#include "helios/reflect/record.h"
#include "sample/common.gen.h"
#include "sample/items.gen.h"
#include "sample/ship.gen.h"

namespace helios::tf::samples {

Result<void> registerSampleTypes(refl::TypeRegistry& registry) {
    HELIOS_TRY(sample::common::registerCommonTypes(registry));
    HELIOS_TRY(sample::ship::registerShipTypes(registry));
    HELIOS_TRY(sample::items::registerItemsTypes(registry));
    return {};
}

std::string sampleHullRecordText() {
    using namespace sample::ship;
    ShipHullDef h;
    h.name.key = "ship.frigate.name";
    h.size = ShipSize::Medium;
    h.faction = sample::common::Faction::Pilots;
    h.mass = 12000.0f;
    // Keys are fixed so the fixture (and its UI goldens) never change between runs.
    ThrusterMount main;
    main.bone = Name("thruster_main");
    main.dir = Vec3(0.0f, 0.0f, -1.0f);
    main.maxForce = 250000.0f;
    h.thrusters.add(Guid(0x66726967'61746500ull, 0x8000'0000'0000'0001ull), main);
    ThrusterMount left;
    left.bone = Name("thruster_left");
    left.dir = Vec3(-1.0f, 0.0f, 0.0f);
    left.maxForce = 40000.0f;
    h.thrusters.add(Guid(0x66726967'61746500ull, 0x8000'0000'0000'0002ull), left);
    ThrusterMount right = left;
    right.bone = Name("thruster_right");
    right.dir = Vec3(1.0f, 0.0f, 0.0f);
    h.thrusters.add(Guid(0x66726967'61746500ull, 0x8000'0000'0000'0003ull), right);
    Hardpoint nose;
    nose.slot = Name("nose");
    nose.size = ShipSize::Small;
    nose.offset = Vec3(0.0f, 0.25f, 4.5f);
    h.hardpoints.push_back(nose);
    Hardpoint dorsal;
    dorsal.slot = Name("dorsal");
    dorsal.size = ShipSize::Medium;
    dorsal.offset = Vec3(0.0f, 1.5f, 0.0f);
    h.hardpoints.push_back(dorsal);
    h.handling.pitchRate = 45.0f;
    h.handling.yawRate = 30.0f;
    h.handling.rollRate = 90.0f;
    h.aiHints[Name("preferredRange")] = 800.0f;
    h.aiHints[Name("retreatHealth")] = 0.25f;
    refl::RecordHeader header;
    header.rid = 0x46726967'61746501ull;  // "Frigate\x01", fixed so references stay stable
    header.name = "hull/frigate";
    header.comment = "Light multirole frigate hull (setting-neutral sample record).";
    return refl::writeRecord(h, header);
}

} // namespace helios::tf::samples
