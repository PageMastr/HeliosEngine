# Link-model symbol audit (02 §1.4 "Symbol audit", ADR-016, WP-0.6c). CTest lint_symbol_audit runs it on
# the images of a modular dev build (HELIOS_MODULAR=ON); its fixtures run in every build.
#
#   cmake -DIMAGES_FILE=<file> -DWORK_DIR=<dir> -DFORMAT=elf -DNM=<nm> [-DPOLICY=<file>] -P symbol_audit.cmake
#   cmake -DIMAGES_FILE=<file> -DWORK_DIR=<dir> -DFORMAT=pe -DDUMPBIN=<dumpbin> [-DPOLICY=<file>] -P ...
#   cmake -DFIXTURE=<dir> [-DPOLICY=<file>] -P symbol_audit.cmake
#
# IMAGES_FILE lists one image per line: "<role> <name> <path>". Roles:
#   group       a link-group library (helios_runtime, helios_client, helios_editor);
#   consumer    an executable or library that links group libraries (tools, tests, apps);
#   game        a game module image (02 §1.4: game_<gem>[_client|_edcore|_edui]). WP-0.6c part 2 adds the
#               Probe module; part 1 audits fixture images;
#   thirdparty  "thirdparty <group> <library> <archive>": a third-party library whose objects the group
#               exports by design (HELIOS_GROUP_EXPORTED_THIRD_PARTY in cmake/HeliosModular.cmake); R1
#               accepts exactly the global symbols that the library's archive defines.
# A FIXTURE directory holds recorded tool output instead: images.txt ("format elf|pe", then "<role> <name>"
# lines, "thirdparty <group> <library>") and, per image, <name>.symtab and <name>.dynsym (`nm -f sysv -p
# --defined-only`, without and with -D; game images need no .dynsym), <library>.archive (the same listing of
# the archive) or <name>.exports (`dumpbin /exports`).
#
# "Mutable data" below is an object symbol (nm class b B d D v V u) in a writable section: .data*, .bss*,
# .tdata*, .tbss*, but not .data.rel.ro* (vtables, typeinfo and constants that hold pointers live there).
#
# ELF images (nm):
#   R1  a group exports only Helios code: every strong exported definition is in namespace helios (or a
#       helios_* namespace, such as generated shader tables) or is a C symbol named helios_*, or comes from
#       a third-party library the group exports by design (role thirdparty). Other third-party archives are
#       linked in hidden (--exclude-libs), so anything else means third-party or global-namespace code was
#       compiled into the group's own objects.
#   R2  each third-party library with process state (the policy's singleton markers) is defined in at most
#       one image: two copies of mimalloc, flecs, Jolt, Luau, Tracy, SDL3, ImGui, volk or netcode split
#       their state (02 §1.4 "Singletons").
#   R3  a consumer or game image has no copy of mutable Helios data that a group defines (a header-defined
#       inline or template static, a function-local static of an inline function): on Windows every image
#       has its own (02 §1.4 "No per-image caches of global state"). Data that an executable imports from
#       a group by copy relocation (in both images' dynamic symbol tables) is one instance, not a copy.
#   R4  a game image defines nothing from flecs, Jolt, Luau, mimalloc or Tracy;
#   R5  a game image defines no mutable data in namespace helios (it imports engine state, never owns it);
#   R6  a game image has no strong definition of a function that a group exports (it imports engine code).
# PE images (dumpbin /exports; a linked image has no symbol table, so R2-R6 run on the ELF build):
#   P1  every undecorated (C) name a group exports is helios_* or one of the CRT and STL header inlines the
#       policy lists (HELIOS_SYMBOL_PE_TOOLCHAIN), and a group exports something.
# A finding that matches the policy's HELIOS_SYMBOL_KNOWN_FINDINGS (rule, owner, regex) is printed as known
# and does not fail the audit; every other finding does. Exits non-zero with one line per finding.

cmake_minimum_required(VERSION 3.28)
if(NOT POLICY)
  get_filename_component(POLICY "${CMAKE_CURRENT_LIST_DIR}/symbol_audit_policy.cmake" ABSOLUTE)
endif()
include("${POLICY}")

set(findings "")
set(known "")
# Records a finding of <rule> about symbol <sym>; a known finding (policy) is kept apart.
function(_fail rule sym msg)
  foreach(entry IN LISTS HELIOS_SYMBOL_KNOWN_FINDINGS)
    # <rule>|<owner>|<regex>: the regex is last because it may contain '|'.
    string(FIND "${entry}" "|" bar1)
    string(SUBSTRING "${entry}" 0 ${bar1} knownRule)
    math(EXPR start "${bar1} + 1")
    string(SUBSTRING "${entry}" ${start} -1 rest)
    string(FIND "${rest}" "|" bar2)
    string(SUBSTRING "${rest}" 0 ${bar2} owner)
    math(EXPR start "${bar2} + 1")
    string(SUBSTRING "${rest}" ${start} -1 knownRegex)
    if(rule STREQUAL knownRule AND NOT sym STREQUAL "" AND sym MATCHES "${knownRegex}")
      list(APPEND known "${msg} [known, owner ${owner}]")
      set(known "${known}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  list(APPEND findings "${msg}")
  set(findings "${findings}" PARENT_SCOPE)
endfunction()

# Sets <out> to the lines of an image listing that match <regex>. <kind> is symtab, dynsym, archive or
# exports. The tool output goes through a file so that file(STRINGS ... REGEX) filters the (large) tables.
function(_listing name path kind regex out)
  if(FIXTURE)
    set(file "${FIXTURE}/${name}.${kind}")
    if(NOT EXISTS "${file}")
      message(FATAL_ERROR "symbol audit fixture ${FIXTURE}: missing ${name}.${kind}")
    endif()
  else()
    set(file "${WORK_DIR}/${name}.${kind}")
    if(kind STREQUAL "exports")
      if(NOT DUMPBIN)
        message(FATAL_ERROR "symbol audit: pass -DDUMPBIN=<dumpbin> for PE images")
      endif()
      set(cmd "${DUMPBIN}" /nologo /exports "${path}")
    else()
      if(NOT NM)
        message(FATAL_ERROR "symbol audit: pass -DNM=<nm> for ELF images")
      endif()
      set(cmd "${NM}" -f sysv -p --defined-only "${path}")
      if(kind STREQUAL "dynsym")
        set(cmd "${NM}" -f sysv -p -D --defined-only "${path}")
      endif()
    endif()
    execute_process(COMMAND ${cmd} RESULT_VARIABLE rc OUTPUT_FILE "${file}" ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "symbol audit: '${cmd}' failed (${rc}): ${err}")
    endif()
  endif()
  file(STRINGS "${file}" lines REGEX "${regex}")
  set(${out} "${lines}" PARENT_SCOPE)
endfunction()

# One `nm -f sysv` line ("name|value|class|type|size|line|section"): sets <class>, <name> (a version
# suffix such as @@VER dropped) and <section>, or leaves <name> empty for a line that is not a symbol.
macro(_sym_line line class name section)
  set(${name} "")
  if("${line}" MATCHES "^([^|]+)\\|[^|]*\\| *([A-Za-z?]) *\\|[^|]*\\|[^|]*\\|[^|]*\\|(.*)$")
    string(STRIP "${CMAKE_MATCH_1}" _symName)
    set(${class} "${CMAKE_MATCH_2}")
    string(STRIP "${CMAKE_MATCH_3}" ${section})
    string(REGEX REPLACE "@.*$" "" ${name} "${_symName}")
  endif()
endmacro()

# True when <class> and <section> describe mutable data (see the header).
macro(_is_mutable_data class section out)
  set(${out} OFF)
  if("${class}" MATCHES "^[bBdDvVu]$" AND "${section}" MATCHES "^\\.(data|bss|tdata|tbss)"
     AND NOT "${section}" MATCHES "^\\.data\\.rel\\.ro")
    set(${out} ON)
  endif()
endmacro()

# --- Images -------------------------------------------------------------------------------------
set(format "")
set(entries "")
set(thirdParty "")
if(FIXTURE)
  file(STRINGS "${FIXTURE}/images.txt" lines)
  foreach(e IN LISTS lines)
    if(e MATCHES "^format (elf|pe)$")
      set(format "${CMAKE_MATCH_1}")
    elseif(e MATCHES "^(group|consumer|game) ([^ ]+)$")
      list(APPEND entries "${CMAKE_MATCH_1}|${CMAKE_MATCH_2}|-")
    elseif(e MATCHES "^thirdparty ([^ ]+) ([^ ]+)$")
      list(APPEND thirdParty "${CMAKE_MATCH_1}|${CMAKE_MATCH_2}|-")
    endif()
  endforeach()
elseif(IMAGES_FILE)
  if(NOT WORK_DIR)
    message(FATAL_ERROR "symbol audit: pass -DWORK_DIR=<dir> for the tool output")
  endif()
  file(MAKE_DIRECTORY "${WORK_DIR}")
  file(STRINGS "${IMAGES_FILE}" lines)
  foreach(e IN LISTS lines)
    if(e MATCHES "^(group|consumer|game) ([^ ]+) (.+)$")
      if(NOT EXISTS "${CMAKE_MATCH_3}")
        message(FATAL_ERROR "symbol audit: ${CMAKE_MATCH_2} (${CMAKE_MATCH_3}) does not exist; build first")
      endif()
      list(APPEND entries "${CMAKE_MATCH_1}|${CMAKE_MATCH_2}|${CMAKE_MATCH_3}")
    elseif(e MATCHES "^thirdparty ([^ ]+) ([^ ]+) (.+)$")
      if(NOT EXISTS "${CMAKE_MATCH_3}")
        message(FATAL_ERROR "symbol audit: ${CMAKE_MATCH_2} (${CMAKE_MATCH_3}) does not exist; build first")
      endif()
      list(APPEND thirdParty "${CMAKE_MATCH_1}|${CMAKE_MATCH_2}|${CMAKE_MATCH_3}")
    endif()
  endforeach()
  set(format "${FORMAT}")
  if(NOT format MATCHES "^(elf|pe)$")
    message(FATAL_ERROR "symbol audit: pass -DFORMAT=elf|pe with IMAGES_FILE")
  endif()
  if(format STREQUAL "pe" AND NOT DUMPBIN)
    message(FATAL_ERROR "symbol audit: dumpbin was not found (MSVC builds read the export tables with it)")
  elseif(format STREQUAL "elf" AND NOT NM)
    message(FATAL_ERROR "symbol audit: nm was not found (ELF builds read the symbol tables with it)")
  endif()
else()
  message(FATAL_ERROR "symbol audit: pass -DIMAGES_FILE=<file> or -DFIXTURE=<dir>")
endif()
if(NOT format)
  message(FATAL_ERROR "symbol audit: ${FIXTURE}/images.txt has no 'format elf|pe' line")
endif()
# Groups first: consumers and games are compared with what the groups define.
set(groups "")
set(others "")
foreach(e IN LISTS entries)
  if(e MATCHES "^group[|]")
    list(APPEND groups "${e}")
  else()
    list(APPEND others "${e}")
  endif()
endforeach()
if(NOT groups)
  message(FATAL_ERROR "symbol audit: no group image listed")
endif()
list(LENGTH entries imageCount)

# --- PE: export tables --------------------------------------------------------------------------
if(format STREQUAL "pe")
  foreach(e IN LISTS groups)
    string(REPLACE "|" ";" e "${e}")
    list(GET e 1 name)
    list(GET e 2 path)
    # "ordinal hint RVA name [= target]" rows; the hint column is absent for ordinal-only exports.
    _listing("${name}" "${path}" exports "^ +[0-9]+ +[0-9A-Fa-f]+ +[0-9A-Fa-f]+ +[^ ]" rows)
    set(n 0)
    foreach(row IN LISTS rows)
      if(row MATCHES "^ +[0-9]+ +[0-9A-Fa-f]+ +[0-9A-Fa-f]+ +([^ ]+)")
        set(sym "${CMAKE_MATCH_1}")
        math(EXPR n "${n} + 1")
        set(toolchain OFF)
        foreach(rx IN LISTS HELIOS_SYMBOL_PE_TOOLCHAIN)
          if(sym MATCHES "${rx}")
            set(toolchain ON)
            break()
          endif()
        endforeach()
        if(NOT sym MATCHES "^\\?" AND NOT sym MATCHES "${HELIOS_SYMBOL_OWNED_REGEX}" AND NOT toolchain)
          _fail(P1 "${sym}" "P1 ${name} exports '${sym}', a C name that is not helios_*: a group exports only Helios objects, never a third-party library (02 §1.4)")
        endif()
      endif()
    endforeach()
    if(n EQUAL 0)
      _fail(P1 "" "P1 ${name}: no exports found (is this a DLL, and did dumpbin's table format change?)")
    endif()
    message(STATUS "symbol audit: ${name} exports ${n} symbols")
  endforeach()
endif()

# --- ELF: symbol tables -------------------------------------------------------------------------
if(format STREQUAL "elf")
  # Markers of the singleton libraries (R2).
  set(markers "")
  set(markerRegex "")
  foreach(entry IN LISTS HELIOS_SYMBOL_SINGLETONS)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 lib)
    list(GET parts 1 marker)
    list(APPEND markers "${marker}")
    string(MD5 k "${marker}")
    set(MARKER_${k} "${lib}")
    if(markerRegex)
      string(APPEND markerRegex "|")
    endif()
    string(APPEND markerRegex "${marker}")
  endforeach()
  set(forbiddenLibs "")
  set(forbiddenRegexes "")
  foreach(entry IN LISTS HELIOS_SYMBOL_GAME_FORBIDDEN)
    string(FIND "${entry}" "|" bar)
    string(SUBSTRING "${entry}" 0 ${bar} lib)
    math(EXPR start "${bar} + 1")
    string(SUBSTRING "${entry}" ${start} -1 rx)
    list(APPEND forbiddenLibs "${lib}")
    list(APPEND forbiddenRegexes "${rx}")
  endforeach()
  list(LENGTH forbiddenLibs nForbidden)
  math(EXPR lastForbidden "${nForbidden} - 1")
  set(anySymbol "\\| *[A-Za-z?] *\\|")
  # Lines of interest outside game images: data named like Helios code, and the singleton markers.
  set(dataOrMarker "^((_Z|helios_)[^|]*\\| *[^|]*\\| *[bBdDvVu] *\\||(${markerRegex}) *\\|)")

  # Third-party libraries a group exports by design: their archives' global definitions (for R1).
  foreach(e IN LISTS thirdParty)
    string(REPLACE "|" ";" e "${e}")
    list(GET e 0 group)
    list(GET e 1 lib)
    list(GET e 2 path)
    _listing("${lib}" "${path}" archive "${anySymbol}" lines)
    foreach(line IN LISTS lines)
      _sym_line("${line}" c s sec)
      if(NOT s STREQUAL "" AND c MATCHES "^[A-Z]$")
        string(MD5 k "${s}")
        set(TP_${group}_${k} "${lib}")
      endif()
    endforeach()
  endforeach()

  # Groups: R1 over the dynamic symbol table. Exported Helios functions (for R6) and exported mutable Helios
  # data (for R3's copy relocations) go into hash sets: MD5-keyed variables.
  foreach(e IN LISTS groups)
    string(REPLACE "|" ";" e "${e}")
    list(GET e 1 name)
    list(GET e 2 path)
    _listing("${name}" "${path}" dynsym "${anySymbol}" lines)
    set(exported 0)
    set(exportedThirdParty 0)
    foreach(line IN LISTS lines)
      _sym_line("${line}" c s sec)
      if(s STREQUAL "")
        continue()
      endif()
      math(EXPR exported "${exported} + 1")
      string(MD5 k "${s}")
      if(s MATCHES "${HELIOS_SYMBOL_OWNED_REGEX}")
        if(c STREQUAL "T")
          set(EXPORT_${k} "${name}")
        endif()
        _is_mutable_data("${c}" "${sec}" mutable)
        if(mutable)
          set(EXPORTED_DATA_${k} "${name}")
        endif()
      elseif(DEFINED TP_${name}_${k})
        math(EXPR exportedThirdParty "${exportedThirdParty} + 1")
      elseif(c MATCHES "^[BDGRST]$" AND NOT s IN_LIST HELIOS_SYMBOL_LINKER_DEFINED)
        _fail(R1 "${s}" "R1 ${name} exports '${s}' (${c}), which is not Helios code: a group exports only Helios objects, and third-party archives stay hidden (02 §1.4)")
      endif()
    endforeach()
    if(exported EQUAL 0)
      _fail(R1 "" "R1 ${name} exports nothing (is it a shared library?)")
    endif()
    message(STATUS "symbol audit: ${name} exports ${exported} symbols (${exportedThirdParty} from third-party libraries it exports by design)")
  endforeach()

  # Every image: R2, R3, R5 (and R4, R6 for game images, which read the whole table).
  foreach(e IN LISTS groups others)
    string(REPLACE "|" ";" e "${e}")
    list(GET e 0 role)
    list(GET e 1 name)
    list(GET e 2 path)
    if(role STREQUAL "game")
      _listing("${name}" "${path}" symtab "${anySymbol}" lines)
    else()
      _listing("${name}" "${path}" symtab "${dataOrMarker}" lines)
    endif()
    # A consumer's dynamic symbol table holds the data it imports by copy relocation (an executable that
    # reads exported engine data, such as log::detail::g_globalLevel through an inline function). ELF
    # binds the group to that copy, so the process has one instance: for R3 it is an import, not a copy.
    # A per-image copy of header-defined state is hidden (consumers build with -fvisibility=hidden) and
    # never appears there.
    set(imported "")
    if(role STREQUAL "consumer")
      _listing("${name}" "${path}" dynsym "^_Z" dynLines)
      foreach(line IN LISTS dynLines)
        _sym_line("${line}" dc ds dsec)
        if(NOT ds STREQUAL "")
          string(MD5 k "${ds}")
          if(DEFINED EXPORTED_DATA_${k})
            list(APPEND imported "${k}")
          endif()
        endif()
      endforeach()
    endif()
    set(forbiddenSeen "")
    foreach(line IN LISTS lines)
      _sym_line("${line}" c s sec)
      if(s STREQUAL "")
        continue()
      endif()
      if(s IN_LIST markers)
        string(MD5 k "${s}")
        set(lib "${MARKER_${k}}")
        string(MD5 lk "${lib}")
        if(DEFINED SINGLETON_${lk} AND NOT SINGLETON_${lk} STREQUAL name)
          _fail(R2 "${s}" "R2 ${lib} is in two images, ${SINGLETON_${lk}} and ${name} ('${s}'): its process state splits (02 §1.4, singletons)")
        else()
          set(SINGLETON_${lk} "${name}")
        endif()
      endif()
      # Guard variables follow their variable; unnamed-namespace names repeat in every TU.
      _is_mutable_data("${c}" "${sec}" mutable)
      if(mutable AND s MATCHES "${HELIOS_SYMBOL_OWNED_REGEX}" AND NOT s MATCHES "^_ZGV"
         AND NOT s MATCHES "_GLOBAL__N_")
        string(MD5 k "${s}")
        if(role STREQUAL "group")
          set(DATA_${k} "${name}")
        elseif(k IN_LIST imported)
          # Imported through a copy relocation (see above): one instance per process.
        elseif(DEFINED DATA_${k})
          _fail(R3 "${s}" "R3 ${name} has its own copy of '${s}' (${c}, ${sec}), which ${DATA_${k}} defines: header-defined state is per image on Windows (02 §1.4, no per-image caches of global state)")
        elseif(role STREQUAL "game")
          _fail(R5 "${s}" "R5 game image ${name} defines Helios data '${s}' (${c}, ${sec}): a game image imports engine state, it never owns any (02 §1.4)")
        endif()
      endif()
      if(role STREQUAL "game")
        foreach(f RANGE ${lastForbidden})
          list(GET forbiddenRegexes ${f} rx)
          if(s MATCHES "${rx}")
            list(GET forbiddenLibs ${f} lib)
            if(NOT lib IN_LIST forbiddenSeen)
              list(APPEND forbiddenSeen "${lib}")
              _fail(R4 "${s}" "R4 game image ${name} defines '${s}' from ${lib}: a game image defines nothing from flecs, Jolt, Luau, mimalloc or Tracy (02 §1.4)")
            endif()
          endif()
        endforeach()
        if(c STREQUAL "T")
          string(MD5 k "${s}")
          if(DEFINED EXPORT_${k})
            _fail(R6 "${s}" "R6 game image ${name} has its own strong definition of '${s}', which ${EXPORT_${k}} exports: a game image imports engine functions (02 §1.4)")
          endif()
        endif()
      endif()
    endforeach()
  endforeach()
endif()

list(LENGTH known nk)
if(nk GREATER 0)
  list(REMOVE_DUPLICATES known)
  list(LENGTH known nk)
  string(REPLACE ";" "\n  " text "${known}")
  message(STATUS "symbol audit: ${nk} known finding(s), each owned by a work package (policy HELIOS_SYMBOL_KNOWN_FINDINGS):\n  ${text}")
endif()
list(LENGTH findings nf)
if(nf GREATER 0)
  string(REPLACE ";" "\n  " text "${findings}")
  message(FATAL_ERROR "symbol audit failed (${nf} finding(s); rules in tools/lint/symbol_audit.cmake, 02 §1.4):\n  ${text}\n")
endif()
message(STATUS "symbol audit: ${imageCount} image(s) passed")
