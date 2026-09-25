#pragma once
// Support header included by every helios-schemac generated file (`*.gen.h`). Not for direct use.

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include "helios/core/guid.h"
#include "helios/core/name.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/math/color.h"
#include "helios/math/quat.h"
#include "helios/math/vec.h"
#include "helios/reflect/codec.h"
#include "helios/reflect/json.h"
#include "helios/reflect/mut.h"
#include "helios/reflect/registry.h"
#include "helios/reflect/tagged.h"
#include "helios/reflect/type_info.h"
#include "helios/reflect/types.h"

// Generated TypeInfo tables use offsetof on structs that hold std::string / std::vector members
// (not standard-layout on every STL). All supported compilers compute these offsets correctly for
// classes without virtual bases; the pragmas silence GCC/Clang's conditionally-supported warning.
#if defined(__GNUC__) || defined(__clang__)
#define HELIOS_GEN_OFFSETOF_BEGIN _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Winvalid-offsetof\"")
#define HELIOS_GEN_OFFSETOF_END _Pragma("GCC diagnostic pop")
#else
#define HELIOS_GEN_OFFSETOF_BEGIN
#define HELIOS_GEN_OFFSETOF_END
#endif
