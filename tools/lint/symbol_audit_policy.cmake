# Policy for the link-model symbol audit (tools/lint/symbol_audit.cmake; 02 §1.4, ADR-016, WP-0.6c).
# Reviewed changes only: an entry here decides which symbols may cross or sit in which image.

# Third-party libraries that hold process state. Each must be defined in exactly one image of a modular
# build (02 §1.4 "Singletons"): <library>|<a symbol only that library defines>. ELF names, mangled.
set(HELIOS_SYMBOL_SINGLETONS
  "mimalloc|mi_malloc"
  "flecs|ecs_init"
  "Jolt|_ZN3JPH7Factory9sInstanceE"
  "Luau|lua_newstate"
  "Tracy|___tracy_emit_zone_begin"
  "SDL3|SDL_Init"
  "Dear ImGui|_ZN5ImGui13CreateContextEP11ImFontAtlas"
  "volk|volkInitialize"
  "netcode|netcode_init")

# Libraries a game image may define nothing from (02 §1.4 "Symbol audit"): <library>|<regex on the raw
# (mangled) name>. A C++ name matches when its qualified name starts in that namespace.
set(HELIOS_SYMBOL_GAME_FORBIDDEN
  "flecs|^(ecs_|flecs_|FLECS_|_Z(T[VISTHW]|GV|ZN?)?N[rVKRO]*5flecs)"
  "Jolt|^_Z(T[VISTHW]|GV|ZN?)?N[rVKRO]*3JPH"
  "Luau|^(lua_|luaL_|luau_|luaU_|luaV_|luaD_|luaG_|luaH_|luaM_|luaO_|luaS_|luaT_|luaZ_|_Z(T[VISTHW]|GV|ZN?)?N[rVKRO]*4Luau)"
  "mimalloc|^_?mi_"
  "Tracy|^(___tracy|_Z(T[VISTHW]|GV|ZN?)?N[rVKRO]*5tracy)")

# A mangled name owned by Helios: an entity of namespace helios (including its vtables, typeinfo, guard
# variables, TLS wrappers, thunks and function-local statics), or a C symbol named helios_*.
set(HELIOS_SYMBOL_OWNED_REGEX
  "^(helios_|_Z(T[VISTTHW]|GV|GR|Th[n0-9]+_|Tv[n0-9]+_[n0-9]+_)?(GV)?Z?N[rVK]*[RO]?6helios)")

# Symbols the ELF linker defines in every shared object.
set(HELIOS_SYMBOL_LINKER_DEFINED _init _fini __bss_start _edata _end __end__ __data_start data_start
  _GLOBAL_OFFSET_TABLE_ _DYNAMIC __dso_handle __TMC_END__)
