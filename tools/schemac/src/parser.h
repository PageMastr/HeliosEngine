#pragma once
// Recursive-descent parser for the .hschema grammar (02 §3.1):
//
//   file   := 'package' qname ';' {'import' string ';'} {decl}
//   decl   := kind Name [':' Base] {hattr} ('{' members '}' | params ['->' type] ';' | '=' hxl ';')
//   members:= { field | rpc | ('client'|'server'|'editor') '{' members '}' }
//   field  := ident ':' type ['=' literal] {'@' attr} [';']      // ';' optional at end of line
//   type   := prim | Name | Name '<' type {',' type} '>' | type '?' | type '[' int ']'
//           | 'enum' '{'…'}' | 'variant' '{' Alt ['{' members '}'] {';' …} '}' | '{' members '}'
//   hattr  := ['@'] ident ['(' args ')'] | 'client->server' | 'server->client' | 'server->server'
//
// Errors are reported with file:line:col; the parser resynchronizes at the next declaration so
// one mistake does not hide the rest of the file.

#include <string_view>

#include "ast.h"
#include "diagnostics.h"

namespace helios::schemac {

/// Parses one source file (already registered with `diags` as `fileIndex`).
FileAst parseFile(std::string_view text, u32 fileIndex, DiagnosticEngine& diags);

/// True for the declaration keywords (enum, flags, struct, ...).
bool isDeclKeyword(std::string_view word) noexcept;
std::string_view declKindName(DeclKindAst kind) noexcept;

} // namespace helios::schemac
