#pragma once
// helios::authority — single-writer authority for the cell (04 §6.1, §3.2): umbrella include.
//
//   types.h        AgId, CellId, RegionId, Epoch, LeaseGen, Owner, FenceResult
//   future.h       Future/Promise for asynchronous control-plane replies (never awaited in a tick)
//   fence.h        IFence: epoch-fenced tree operations (advance, park, join, leave, bulk, recovery)
//   memory_fence.h InMemoryFence: the in-process fence used by tests, bots and PIE
//   ag_table.h     AgTable: the AGs a zone instance owns, fence gate, released handling
//   lease.h        LeaseHolder: region lease generations and the holder rule (05 §1.4.2)
//   zone_clock.h   ZoneClock + TiDiController: dilatable tick clock (04 §3.2)

#include "helios/authority/ag_table.h"
#include "helios/authority/fence.h"
#include "helios/authority/future.h"
#include "helios/authority/lease.h"
#include "helios/authority/memory_fence.h"
#include "helios/authority/types.h"
#include "helios/authority/zone_clock.h"
