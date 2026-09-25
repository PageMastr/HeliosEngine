#pragma once
// flecs OS API -> Helios platform services (ADR-011).
//
// installFlecsOsApi() replaces flecs' process-global ecs_os_api once, before the first flecs world:
//   * memory      malloc/calloc/realloc/free/strdup -> ecsHeap() (mimalloc v3 heap, tag "ECS");
//   * logging     flecs levels -> HELIOS_LOG_* on channel "ECS" (>0 trace, 0 debug, -2 warn,
//                 -3 error, -4 fatal as error; flecs then calls abort);
//   * abort       reported through the Helios assert handler (kind "FLECS"), then flush + abort;
//   * time        now/get_time -> monotonic clock, sleep -> helios::sleepNanos;
//   * threads     thread_new/join -> helios::Thread (named "flecs worker"), thread_self -> OS tid;
//   * tasks       task_new/join -> JobSystem jobs of the world that owns the stage (see below).
// Mutexes and condition variables keep flecs' own Win32/POSIX implementations.
//
// Task threads: flecs 4.1.6 supports two worker mechanisms, ecs_set_threads (long-lived OS
// threads) and ecs_set_task_threads (short-lived tasks created with ecs_os_api.task_new_ at the
// start of every ecs_progress and joined with task_join_ at its end). Helios uses the task
// mechanism: task_new_ looks up the owning World from the stage (ecs_get_world + ecs_get_ctx) and
// submits the flecs worker loop as a High-priority job; task_join_ waits on the job's counter with
// JobSystem::wait (helping). flecs workers block on flecs' condition variables between pipeline
// sync points, so each task occupies a worker for the whole ecs_progress: World clamps the task
// count to the worker count (and asserts it is not called from a worker, which would leave one
// fewer thread to run them). Helios systems do not use this path — the Helios scheduler
// (system.h) is job-native and never blocks workers — it exists for flecs-native systems/addons.
//
// Threading: installFlecsOsApi() is idempotent and thread-safe; the hooks are thread-safe.

#include "helios/core/log.h"
#include "helios/core/types.h"

namespace helios::ecs {

/// Log channel for helios::ecs and everything flecs reports.
HELIOS_LOG_CHANNEL(LogEcs, "ECS");

/// Installs the Helios OS API (idempotent). Called by World's constructor; call it yourself before
/// using flecs directly. Returns false if flecs' OS API was already initialized by someone else
/// (then flecs keeps its defaults and a warning is logged).
bool installFlecsOsApi() noexcept;
/// True once installFlecsOsApi() succeeded.
bool flecsOsApiInstalled() noexcept;

struct FlecsOsApiStats {
    u64 mallocs = 0;
    u64 reallocs = 0;
    u64 frees = 0;
    u64 tasksStarted = 0; ///< flecs task threads run as jobs.
    u64 tasksJoined = 0;
    u64 threadsStarted = 0;
    u64 logWarnings = 0;
    u64 logErrors = 0;
};
FlecsOsApiStats flecsOsApiStats() noexcept;

} // namespace helios::ecs
