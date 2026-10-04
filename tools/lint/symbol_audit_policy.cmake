# Policy for the link-model symbol audit (tools/lint/symbol_audit.cmake; 02 §1.4, ADR-016, WP-0.6c).
# Reviewed changes only: an entry here decides which symbols may cross or sit in which image.

# Third-party libraries that hold process state. Each must be defined in exactly one image of a modular
# build (02 §1.4 "Singletons"): <library>|<a symbol only that library defines>. ELF names, mangled (Luau's
# C API has C++ linkage; the Tracy marker exists with and without TRACY_ENABLE).
set(HELIOS_SYMBOL_SINGLETONS
  "mimalloc|mi_malloc"
  "flecs|ecs_init"
  "Jolt|_ZN3JPH7Factory9sInstanceE"
  "Luau|_Z12lua_newstatePFPvS_S_mmES_"
  "Tracy|_ZN5tracy13SetThreadNameEPKc"
  "SDL3|SDL_Init"
  "Dear ImGui|_ZN5ImGui13CreateContextEP11ImFontAtlas"
  "volk|volkInitialize"
  "netcode|netcode_init")

# Libraries a game image may define nothing from (02 §1.4 "Symbol audit"): <library>|<regex on the raw
# (mangled) name>. A C++ name matches when its qualified name starts in that namespace.
set(HELIOS_SYMBOL_GAME_FORBIDDEN
  "flecs|^(ecs_|flecs_|FLECS_|_Z(T[VISTHW]|GV|ZN?)?N[rVKRO]*5flecs)"
  "Jolt|^_Z(T[VISTHW]|GV|ZN?)?N[rVKRO]*3JPH"
  "Luau|^(lua[A-Za-z]?_|_Z[0-9]+lua[A-Za-z]?_|_Z(T[VISTHW]|GV|ZN?)?N[rVKRO]*4Luau)"
  "mimalloc|^_?mi_"
  "Tracy|^(___tracy|_Z(T[VISTHW]|GV|ZN?)?N[rVKRO]*5tracy)")

# A mangled name owned by Helios: an entity of namespace helios (including its vtables, typeinfo, guard
# variables, TLS wrappers, thunks and function-local statics), or a C symbol named helios_*.
set(HELIOS_SYMBOL_OWNED_REGEX
  "^(helios_|_Z(T[VISTTHW]|GV|GR|Th[n0-9]+_|Tv[n0-9]+_[n0-9]+_)?(GV)?Z?N[rVK]*[RO]?6helios)")

# Symbols the ELF linker defines in every shared object.
set(HELIOS_SYMBOL_LINKER_DEFINED _init _fini __bss_start _edata _end __end__ __data_start data_start
  _GLOBAL_OFFSET_TABLE_ _DYNAMIC __dso_handle __TMC_END__)

# Third-party code a group exports by design (R1): <group>|<library>|<regex on the raw (mangled) name>.
# Keep in step with HELIOS_GROUP_EXPORTED_THIRD_PARTY in cmake/HeliosModular.cmake.
#   Luau's C API (lua.h, lualib.h: lua_*, luaL_*, luau_*; C++ linkage): engine/script's public API is
#   built on it, so the other groups and generated binding glue call the one VM in helios_runtime. Luau's
#   internal functions are hidden (LUAI_FUNC) and never exported.
set(HELIOS_SYMBOL_GROUP_THIRD_PARTY
  "helios_runtime|Luau VM|^_Z[0-9]+lua[uL]?_")
