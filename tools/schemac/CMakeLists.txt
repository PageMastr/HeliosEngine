# tools/schemac — helios-schemac, the Helios schema compiler (02 §3.5, ADR-004).
# Depends only on helios::core and yyjson so it can be built first and for the build host.

set(HELIOS_SCHEMAC_EMBEDDED ${CMAKE_CURRENT_BINARY_DIR}/schemac_go_runtime.cpp)
add_custom_command(
  OUTPUT ${HELIOS_SCHEMAC_EMBEDDED}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/runtime/helios_runtime.go
          -DOUTPUT=${HELIOS_SCHEMAC_EMBEDDED} -DSYMBOL=kGoRuntimeSource -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/runtime/helios_runtime.go ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
  COMMENT "Embedding the Go schema runtime into helios-schemac"
  VERBATIM)

add_library(helios_schemac_lib STATIC
  src/cli.cpp
  src/compiler.cpp
  src/diagnostics.cpp
  src/gen_cpp.cpp
  src/gen_go.cpp
  src/gen_json.cpp
  src/gen_lint.cpp
  src/gen_luau.cpp
  src/gen_repl.cpp
  src/gen_sql.cpp
  src/json_out.cpp
  src/lexer.cpp
  src/lock.cpp
  src/lock_mutex.cpp
  src/model.cpp
  src/parser.cpp
  src/samples.cpp
  src/sema.cpp
  src/text.cpp
  ${HELIOS_SCHEMAC_EMBEDDED})
target_include_directories(helios_schemac_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(helios_schemac_lib PUBLIC helios::core PRIVATE helios::tp::yyjson)
set_target_properties(helios_schemac_lib PROPERTIES FOLDER tools)
helios_apply_warnings(helios_schemac_lib)

add_executable(helios-schemac src/main.cpp)
target_link_libraries(helios-schemac PRIVATE helios_schemac_lib)
set_target_properties(helios-schemac PROPERTIES FOLDER tools)
helios_apply_warnings(helios-schemac)
# Modular dev builds (ADR-016): the generator runs during the build and its output feeds a module of
# helios_runtime, so it links core's objects itself instead of that library (cmake/HeliosModular.cmake).
helios_standalone_tool(helios_schemac_lib)
helios_standalone_tool(helios-schemac MODULES core)

# ---------------------------------------------------------------------------------------------
# Tests: parser/sema/lock/golden tests in-process, plus the sample schemas compiled through
# helios_schema() (C++ round trips, reflection, and the Go cross-language check).
# ---------------------------------------------------------------------------------------------
if(HELIOS_BUILD_TESTS)
  file(GLOB HELIOS_SCHEMAC_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  # The generated Luau glue runs on the real script host, and Luau.Analysis type-checks schema.d.luau.
  # helios::net: test_lint.cpp pins the size.unreliable budget to engine/net's wire limits.
  helios_test(schemac_tests SOURCES ${HELIOS_SCHEMAC_TEST_SOURCES}
    DEPS helios_schemac_lib helios::reflect helios::script helios::tp::luau_analysis helios::net)
  set(HELIOS_SAMPLE_SCHEMAS
    ${PROJECT_SOURCE_DIR}/schemas/sample/common.hschema
    ${PROJECT_SOURCE_DIR}/schemas/sample/ship.hschema
    ${PROJECT_SOURCE_DIR}/schemas/sample/items.hschema)
  set(HELIOS_SAMPLE_GO_OUT ${CMAKE_CURRENT_BINARY_DIR}/go/sample)
  helios_schema(schemac_tests
    FILES ${HELIOS_SAMPLE_SCHEMAS}
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/schemas
    LOCK ${PROJECT_SOURCE_DIR}/schemas/sample/schema.lock.jsonc
    GO_OUT ${HELIOS_SAMPLE_GO_OUT}
    GO_PACKAGE sample
    JSON_OUT ${CMAKE_CURRENT_BINARY_DIR}/sample.schema.json
    LUAU_OUT ${CMAKE_CURRENT_BINARY_DIR}/luau/sample
    REPL
    SAMPLES)
  # Golden fixture: its generated C++ and Go are compiled too (the golden test compares the text).
  set(HELIOS_GOLDEN_GO_OUT ${CMAKE_CURRENT_BINARY_DIR}/go/golden)
  helios_schema(schemac_tests
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/golden.hschema
    INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden
    LOCK ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/golden.lock.jsonc
    GO_OUT ${HELIOS_GOLDEN_GO_OUT}
    LUAU_OUT ${CMAKE_CURRENT_BINARY_DIR}/luau/golden
    REPL
    SAMPLES)
  # Replication edge cases (tests/repl): a package with types named like the runtime's, and f32 range
  # bounds that are not f32 values. Compiling its generated code is part of the test.
  helios_schema(schemac_tests
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/tests/repl/repl_edges.hschema
    INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/tests/repl
    LOCK ${CMAKE_CURRENT_SOURCE_DIR}/tests/repl/repl_edges.lock.jsonc
    REPL)
  find_program(HELIOS_GO_EXECUTABLE go HINTS $ENV{GOROOT}/bin /usr/local/go/bin "C:/Program Files/Go/bin")
  if(NOT HELIOS_GO_EXECUTABLE)
    set(HELIOS_GO_EXECUTABLE "")
  endif()
  target_compile_definitions(schemac_tests PRIVATE
    HELIOS_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    HELIOS_SCHEMAC_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
    HELIOS_SAMPLE_GO_DIR="${HELIOS_SAMPLE_GO_OUT}"
    HELIOS_GOLDEN_GO_DIR="${HELIOS_GOLDEN_GO_OUT}"
    HELIOS_SAMPLE_SCHEMA_JSON="${CMAKE_CURRENT_BINARY_DIR}/sample.schema.json"
    HELIOS_SAMPLE_LUAU_DIR="${CMAKE_CURRENT_BINARY_DIR}/luau/sample"
    HELIOS_GOLDEN_LUAU_DIR="${CMAKE_CURRENT_BINARY_DIR}/luau/golden"
    HELIOS_SCRIPT_DEFS_PATH="${PROJECT_SOURCE_DIR}/engine/script/defs/helios.d.luau"
    HELIOS_GO_EXECUTABLE="${HELIOS_GO_EXECUTABLE}"
    HELIOS_CMAKE_COMMAND="${CMAKE_COMMAND}"
    HELIOS_TEST_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_work")
  set_tests_properties(schemac_tests PROPERTIES TIMEOUT 600)
  # The size budgets as a gate (02 §3.5: `--emit lint` is CI's): the production schemas
  # (schemas/gameplay, compiled into engine/gameplay) must have no size.* finding with warnings as
  # errors. Read-only: --check-lock fails on a stale lock instead of writing it. The sample schemas
  # have known findings (pinned by the corpus golden); the seeded-violation fixture runs the same
  # command over them and must fail, so a gate that stops reporting fails too. Host-only, like
  # schemac_sql_postgres (helios-schemac runs on the build host).
  if(NOT CMAKE_CROSSCOMPILING)
    file(GLOB HELIOS_SCHEMAC_LINT_GAMEPLAY CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/schemas/gameplay/*.hschema)
    list(SORT HELIOS_SCHEMAC_LINT_GAMEPLAY)
    add_test(NAME lint_schemac_size_gameplay
      COMMAND helios-schemac --emit lint --Werror -I ${PROJECT_SOURCE_DIR}/schemas
              --lock ${PROJECT_SOURCE_DIR}/schemas/gameplay/schema.lock.jsonc --check-lock
              --lint-out ${CMAKE_CURRENT_BINARY_DIR}/lint/gameplay.lint.json ${HELIOS_SCHEMAC_LINT_GAMEPLAY})
    set(_lintSample --emit lint --Werror -I ${PROJECT_SOURCE_DIR}/schemas --lock ${PROJECT_SOURCE_DIR}/schemas/sample/schema.lock.jsonc
                    --check-lock --lint-out ${CMAKE_CURRENT_BINARY_DIR}/lint/sample.lint.json ${HELIOS_SAMPLE_SCHEMAS})
    set(_lintArgs -DARG0=$<TARGET_FILE:helios-schemac>)
    set(_i 1)
    foreach(_arg IN LISTS _lintSample)
      list(APPEND _lintArgs -DARG${_i}=${_arg})
      math(EXPR _i "${_i} + 1")
    endforeach()
    add_test(NAME lint_schemac_size_fixture_sample
      COMMAND ${CMAKE_COMMAND} "-DEXPECT=error: \\[size\\.unbounded\\]" -DNARGS=${_i} ${_lintArgs}
              -P ${PROJECT_SOURCE_DIR}/tools/lint/expect_fail.cmake)
    set_tests_properties(lint_schemac_size_gameplay lint_schemac_size_fixture_sample PROPERTIES LABELS lint)
  endif()
  # --emit sql on a real PostgreSQL (tests/sql_postgres.cmake): the migration stubs of two releases reach
  # the snapshot, Down returns, the committed SQL goldens apply. Skipped without PostgreSQL binaries
  # (PGBIN or /usr/lib/postgresql/*/bin); a host-only test (helios-schemac runs on the build host). Not
  # registered on Windows yet: starting a private server on the hosted Windows runners is unverified.
  if(NOT CMAKE_CROSSCOMPILING AND NOT WIN32)
    set(HELIOS_SCHEMAC_GOLDEN ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden)
    add_test(NAME schemac_sql_postgres
      COMMAND ${CMAKE_COMMAND} -DSCHEMAC=$<TARGET_FILE:helios-schemac> -DSQL_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/sql
              -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/sql_postgres
              "-DSNAPSHOTS=${HELIOS_SCHEMAC_GOLDEN}/svc_golden.schema.sql.expected;${HELIOS_SCHEMAC_GOLDEN}/corpus/sample/svc_character.schema.sql.expected"
              -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/sql_postgres.cmake)
    set_tests_properties(schemac_sql_postgres PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 300)
  endif()
endif()
