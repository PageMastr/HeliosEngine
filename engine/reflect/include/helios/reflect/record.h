#pragma once
// Record files (02 §3.3): one `.hrec` JSONC file per record, carrying header keys before the
// fields in canonical order:
//
//   {
//     "$rid": 4611686018427387905,   // 63-bit RecordId, minted once (mintRecordId) and never changed
//     "$name": "hull/kestrel",       // renameable; references use $rid
//     "$parent": "hull/base",        // optional record-template parent, resolved at cook time
//     "$comment": "notes that survive editing",
//     "mass": 12000,
//     ...
//   }
//
// Threading: stateless.

#include <string>
#include <string_view>

#include "helios/core/result.h"
#include "helios/reflect/json.h"
#include "helios/reflect/serialize.h"
#include "helios/reflect/type_info.h"
#include "helios/reflect/types.h"

namespace helios::refl {

struct RecordHeader {
    RecordId rid = 0;
    std::string name;
    std::string parent;
    std::string comment;
    friend bool operator==(const RecordHeader&, const RecordHeader&) = default;
};

/// A RecordId is valid when it is non-zero and fits in 63 bits.
constexpr bool isValidRecordId(RecordId id) noexcept { return id != 0 && (id & ~kRecordIdMask) == 0; }

/// Parses a record file into `header` and `object` (start `object` at its default value).
/// Fails when `$rid` is missing or invalid.
Result<void> readRecord(const TypeInfo& type, void* object, std::string_view text, RecordHeader& header, ReadCtx& ctx);
/// Canonical record file text.
std::string writeRecord(const TypeInfo& type, const void* object, const RecordHeader& header);

template <class T>
Result<void> readRecord(std::string_view text, T& object, RecordHeader& header, ReadCtx& ctx) {
    return readRecord(typeOf<T>(), &object, text, header, ctx);
}
template <class T>
std::string writeRecord(const T& object, const RecordHeader& header) {
    return writeRecord(typeOf<T>(), &object, header);
}

} // namespace helios::refl
