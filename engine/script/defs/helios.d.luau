--!strict
-- Helios Luau API type definitions (hand-written Phase 0 stub; WP-1.6 generates the scriptlib part
-- from `@script` schema functions). Consumed by luau-lsp (`luau-lsp.types.definitionFiles`) and by
-- Luau.Analysis in the editor; CI parses it and checks that every built-in listed by
-- ScriptVm::apiManifest() is declared here (engine/script/tests/test_defs.cpp).
--
-- Sandbox (02 §7.4): `io`, `os`, `debug`, `loadstring`, `getfenv`, `setfenv` and
-- `math.randomseed` do not exist. On cells `gcinfo` is also removed (Luau 0.739 has no
-- `collectgarbage`) and `setmetatable` rejects metatables with `__mode`. Libraries and globals
-- are frozen; every module has its own environment. `math.random` keeps its standard signature
-- but draws from a seeded, deterministic stream. Builtins whose work grows with their input or
-- output charge fuel per item or byte (string.rep/format/gsub/find/match/gmatch/split/sub/upper/
-- lower/reverse/pack/unpack, table.concat/sort/move/create/clone/find/insert/remove/clear/maxn,
-- buffer.fill/copy/create/fromstring/tostring/readstring/writestring, utf8.len/offset); on cells
-- pattern subjects are capped at 64 KiB. print keeps at most 4 KiB per line.
--
-- Budgets: a resume that exceeds its hard budget is killed with a sticky `Killed` error that pcall
-- cannot swallow. Scripts yield only at explicit calls: wait, task.wait, task.yield,
-- task.checkpoint, coroutine.yield and asynchronous host calls.

-- A typed error raised by the host: `pcall` returns it as the error value. `code` is one of
-- "Runtime", "OutOfMemory", "StaleHandle", "Killed", "Cancelled", "ModuleDisabled", "NotFound",
-- "HostError", "InvalidState"; kills also carry `reason` ("Fuel", "WallBudget", "WallBackstop").
declare extern type ScriptError with
    code: string
    message: string
    reason: string?
end

-- Handle to a scheduler task (a coroutine). Stale once the task finished.
declare extern type Task with
end

-- Frame-local double-precision position (metres). Never a float `vector` (ADR-005).
declare extern type WorldPos with
    x: number
    y: number
    z: number
    frame: number
    -- Distance to `other` (same frame required).
    function distance(self, other: WorldPos): number
    -- Distance from the frame origin.
    function length(self): number
    -- This position moved by (dx, dy, dz).
    function offset(self, dx: number, dy: number, dz: number): WorldPos
    -- Linear interpolation towards `other` (same frame).
    function lerp(self, other: WorldPos, t: number): WorldPos
    function __add(self, other: WorldPos): WorldPos
    function __sub(self, other: WorldPos): WorldPos
    function __mul(self, k: number): WorldPos
    function __div(self, k: number): WorldPos
    function __unm(self): WorldPos
end

declare WorldPos: {
    -- New position in `frame` (default 0, the root frame).
    new: (x: number, y: number, z: number, frame: number?) -> WorldPos,
}

-- Suspends the calling task for `seconds` of zone time (time dilation applies); returns the zone
-- seconds actually elapsed. wait(0) resumes on the next tick. Only in a task's own coroutine.
declare function wait(seconds: number?): number

-- Returns the exports of module `name`, running its chunk once (its top level cannot yield).
declare function require(name: string): any

-- Writes the arguments to the script log (tab-separated, via tostring).
declare function print(...: any): ()

declare task: {
    -- Same as wait().
    wait: (seconds: number?) -> number,
    -- Suspends the task until the next tick.
    yield: () -> (),
    -- Cooperative long work: yields (returning true after resuming) once the resume is past its
    -- soft budget; returns false immediately otherwise or where yielding is impossible.
    checkpoint: () -> boolean,
    -- Starts `fn(...)` as a new task on a later tick; it inherits the caller's module and owner.
    spawn: <A...>(fn: (A...) -> ...any, A...) -> Task,
    -- Cancels a task that is not running; false if it already finished.
    cancel: (t: Task) -> boolean,
    -- "ready", "waiting", "awaiting", "running" or "dead".
    status: (t: Task) -> string,
    -- The running task, or nil inside callbacks and module loads.
    current: () -> Task?,
    -- Zone time in seconds.
    now: () -> number,
    -- Fuel used so far by the current resume.
    fuel: () -> number,
}

-- math.random(): number in [0, 1); math.random(m): integer in [1, m]; math.random(m, n): integer
-- in [m, n] — from the VM's deterministic stream (declared by the standard library typings).
