#pragma once
// Umbrella header for helios::ecs (World, systems, command buffers, dirty tracking, IDs, heaps).
// Threading rules are documented per header; see engine/ecs/README.md for the overview.

#include "helios/ecs/command_buffer.h"
#include "helios/ecs/component.h"
#include "helios/ecs/dirty.h"
#include "helios/ecs/entity_id.h"
#include "helios/ecs/heap.h"
#include "helios/ecs/os_api.h"
#include "helios/ecs/registry.h"
#include "helios/ecs/system.h"
#include "helios/ecs/types.h"
#include "helios/ecs/world.h"
