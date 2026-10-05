#pragma once
// Internal: a default-constructed object of a reflected type (meta.cpp reads settings into one, and
// importer.cpp reads a settings type's defaults from one).
//
// Threading: an object is used by one thread at a time.

#include <algorithm>
#include <cstddef>
#include <new>

#include "helios/core/types.h"
#include "helios/reflect/type_info.h"

namespace helios::assetpipe::detail {

/// Owns one object of `type`, default-constructed through its ops and destroyed with it. The type must
/// have ops->construct and ops->destruct (ImporterRegistry::add checks a settings type).
class TypedObject {
public:
    explicit TypedObject(const refl::TypeInfo& type)
        : m_type(type), m_align(std::max<usize>(type.align, alignof(std::max_align_t))),
          m_ptr(::operator new(std::max<usize>(type.size, 1), std::align_val_t(m_align))) {
        type.ops->construct(m_ptr);
    }
    ~TypedObject() {
        m_type.ops->destruct(m_ptr);
        ::operator delete(m_ptr, std::align_val_t(m_align));
    }
    TypedObject(const TypedObject&) = delete;
    TypedObject& operator=(const TypedObject&) = delete;
    void* get() const noexcept { return m_ptr; }

private:
    const refl::TypeInfo& m_type;
    usize m_align;
    void* m_ptr;
};

} // namespace helios::assetpipe::detail
