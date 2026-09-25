#pragma once
// Go spellings shared by the Go generator and the sample generator.

#include <string>

#include "model.h"

namespace helios::schemac {

/// Go type expression for a schema type ("[]Keyed[ShipHullDefThruster]", "map[string]float32").
std::string goTypeName(const Type* t);
/// Go field name ("maxForce" -> "MaxForce").
std::string goFieldName(const std::string& name);
/// Go constant of an enum value ("ShipSizeLarge").
std::string goEnumConst(const Decl* enumDecl, const std::string& value);
/// Go name of a record's reference type ("ShipHullRef").
std::string goRecordRefName(const Decl* record);
/// Go literal for a resolved default value.
std::string goValueLiteral(const Value& v, const Type* t);
/// Go expression producing the implicit default of `t` (e.g. NewInner(), Quat{0, 0, 0, 1}, 0).
std::string goZeroValue(const Type* t);

/// C++ spellings (gen_cpp.cpp).
std::string cppTypeName(const Type* t);
std::string cppValueLiteral(const Value& v, const Type* t);

} // namespace helios::schemac
