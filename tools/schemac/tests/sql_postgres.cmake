# CTest schemac_sql_postgres: `--emit sql` output on a real PostgreSQL (02 §3.5, 05 §3).
#
#   cmake -DSCHEMAC=<helios-schemac> -DSQL_DIR=<tests/sql> -DWORK_DIR=<dir> -DSNAPSHOTS=<a.sql;b.sql> -P sql_postgres.cmake
#
# 1. Compiles tests/sql/v1 and then v2 against one lock, as two releases would.
# 2. Database "chain": v1's migration stub (creates the tables), then v2's stub, then v2's contract
#    comments (release N+2: a removed column and a removed table). Database "snap": v2's snapshot.
#    Their pg_dump --schema-only must match, and in both a row that takes the defaults holds the
#    control characters of a string default.
# 3. Database "down": both stubs' Up, then v2's Down, must match v1's snapshot.
# 4. Every snapshot in SNAPSHOTS (the committed goldens) applies to an empty database.
# Prints "SKIPPED:" (the CTest skip pattern) when no PostgreSQL server binaries are found. As root, the
# server runs as `nobody` through runuser (PostgreSQL refuses root), which needs a WORK_DIR it can reach.

cmake_minimum_required(VERSION 3.28)
foreach(v SCHEMAC SQL_DIR WORK_DIR)
  if(NOT ${v})
    message(FATAL_ERROR "sql_postgres: -D${v}=... is required")
  endif()
endforeach()

set(hints "$ENV{PGBIN}")
file(GLOB pg_versions LIST_DIRECTORIES true "/usr/lib/postgresql/*/bin" "C:/Program Files/PostgreSQL/*/bin")
list(SORT pg_versions COMPARE NATURAL ORDER DESCENDING)
list(APPEND hints ${pg_versions})
foreach(tool initdb pg_ctl psql pg_dump)
  find_program(PG_${tool} ${tool} HINTS ${hints} NO_CACHE)
  if(NOT PG_${tool})
    message(STATUS "SKIPPED: no PostgreSQL ${tool} found (set PGBIN to its bin directory)")
    return()
  endif()
endforeach()

set(run_as "")
if(CMAKE_HOST_UNIX)
  execute_process(COMMAND id -u OUTPUT_VARIABLE uid OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(uid STREQUAL "0")
    find_program(RUNUSER runuser NO_CACHE)
    if(NOT RUNUSER)
      message(STATUS "SKIPPED: running as root without runuser (PostgreSQL refuses to run as root)")
      return()
    endif()
    set(run_as ${RUNUSER} -u nobody --)
  endif()
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
if(run_as)
  execute_process(COMMAND chown -R nobody "${WORK_DIR}")
endif()
set(data "${WORK_DIR}/data")
set(port 55439)
# A Unix socket in WORK_DIR (no TCP port shared with other builds); Windows uses loopback TCP.
if(CMAKE_HOST_WIN32)
  set(pg_host 127.0.0.1)
  set(server_options "-c listen_addresses=127.0.0.1 -p ${port}")
else()
  set(pg_host "${WORK_DIR}")
  set(server_options "-k ${WORK_DIR} -c listen_addresses= -p ${port}")
endif()
set(errors "")

macro(check_ok rc what)
  if(NOT ${rc} EQUAL 0)
    list(APPEND errors "${what}")
  endif()
endmacro()

# Runs psql on database `db` with the file `sql_file` (ON_ERROR_STOP).
function(psql_file db sql_file out_rc)
  execute_process(COMMAND ${run_as} ${PG_psql} -X -q -v ON_ERROR_STOP=1 -h "${pg_host}" -p ${port} -U postgres -d ${db} -f "${sql_file}"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(STATUS "psql ${db} < ${sql_file} failed:\n${out}${err}")
  endif()
  set(${out_rc} ${rc} PARENT_SCOPE)
endfunction()

function(psql_command db command out_rc)
  file(WRITE "${WORK_DIR}/cmd.sql" "${command}\n")
  psql_file(${db} "${WORK_DIR}/cmd.sql" rc)
  set(${out_rc} ${rc} PARENT_SCOPE)
endfunction()

# Writes the Up or Down section of goose stub `stub` to `out_file`.
function(goose_section stub section out_file)
  file(READ "${stub}" text)
  string(FIND "${text}" "-- +goose Down" down)
  if(section STREQUAL "Up")
    string(SUBSTRING "${text}" 0 ${down} part)
  else()
    string(SUBSTRING "${text}" ${down} -1 part)
  endif()
  file(WRITE "${out_file}" "${part}")
endfunction()

# pg_dump --schema-only of `schema` in `db`, without comments and session settings.
function(dump db schema out_var)
  execute_process(COMMAND ${run_as} ${PG_pg_dump} -h "${pg_host}" -p ${port} -U postgres -s -n ${schema} --no-owner ${db}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE text ERROR_VARIABLE err)
  if(NOT rc EQUAL 0 OR NOT text MATCHES "CREATE TABLE ${schema}\\.")
    set(errors ${errors} "pg_dump ${db}: ${err} (no tables)" PARENT_SCOPE)
  endif()
  string(REGEX REPLACE "--[^\n]*\n" "" text "${text}")
  string(REGEX REPLACE "(SET|SELECT pg_catalog\\.set_config)[^\n]*\n" "" text "${text}")
  string(REGEX REPLACE "\\\\(un)?restrict [^\n]*\n" "" text "${text}")
  string(REGEX REPLACE "\n\n+" "\n" text "${text}")
  file(WRITE "${WORK_DIR}/${db}.dump" "${text}")
  set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

execute_process(COMMAND ${run_as} ${PG_initdb} -D "${data}" -U postgres --auth=trust -E UTF8 RESULT_VARIABLE rc OUTPUT_QUIET
                ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "sql_postgres: initdb failed: ${err}")
endif()
execute_process(COMMAND ${run_as} ${PG_pg_ctl} -D "${data}" -l "${WORK_DIR}/server.log" -w
                        -o "${server_options}" start
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  file(READ "${WORK_DIR}/server.log" log)
  message(FATAL_ERROR "sql_postgres: the server did not start: ${err}\n${log}")
endif()

# --- 1. two releases against one lock ----------------------------------------------------------
foreach(release v1 v2)
  execute_process(COMMAND "${SCHEMAC}" -I "${SQL_DIR}/${release}" --lock "${WORK_DIR}/lock.jsonc" --emit sql --sql-out "${WORK_DIR}/${release}"
                          --quiet "${SQL_DIR}/${release}/tables.hschema"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    list(APPEND errors "helios-schemac ${release}: ${out}${err}")
  endif()
endforeach()
set(v1 "${WORK_DIR}/v1/svc_sqltest")
set(v2 "${WORK_DIR}/v2/svc_sqltest")

if(NOT errors)
  foreach(db chain snap down snap1 golden)
    psql_command(postgres "CREATE DATABASE ${db};" rc)
    check_ok(rc "create database ${db}")
  endforeach()
  # --- 2. the migrations reach the snapshot ----------------------------------------------------
  psql_command(chain "CREATE SCHEMA svc_sqltest;" rc) # (services/migrations creates the schema before goose runs)
  goose_section("${v1}/migration.sql" Up "${WORK_DIR}/v1.up.sql")
  goose_section("${v2}/migration.sql" Up "${WORK_DIR}/v2.up.sql")
  goose_section("${v2}/migration.sql" Down "${WORK_DIR}/v2.down.sql")
  psql_file(chain "${WORK_DIR}/v1.up.sql" rc)
  check_ok(rc "v1 migration Up")
  psql_file(chain "${WORK_DIR}/v2.up.sql" rc)
  check_ok(rc "v2 migration Up")
  # The contract release runs the commented DROPs.
  file(STRINGS "${v2}/migration.sql" contract REGEX "^--   (ALTER TABLE .* DROP COLUMN |DROP TABLE )")
  set(contract_sql "")
  foreach(line IN LISTS contract)
    string(REGEX REPLACE "^--   ([^;]*;).*" "\\1" stmt "${line}")
    string(APPEND contract_sql "${stmt}\n")
  endforeach()
  if(NOT contract_sql MATCHES "DROP COLUMN legacy;")
    list(APPEND errors "v2's stub has no contract step for the removed field")
  endif()
  if(NOT contract_sql MATCHES "DROP TABLE svc_sqltest\\.retired;")
    list(APPEND errors "v2's stub has no contract step for the removed table")
  endif()
  psql_command(chain "${contract_sql}" rc)
  check_ok(rc "contract step")
  psql_file(snap "${v2}/schema.sql" rc)
  check_ok(rc "v2 snapshot")
  dump(chain svc_sqltest chain_dump)
  dump(snap svc_sqltest snap_dump)
  if(NOT chain_dump STREQUAL snap_dump)
    list(APPEND errors "v1 + v2 migrations differ from the v2 snapshot (diff ${WORK_DIR}/chain.dump ${WORK_DIR}/snap.dump)")
  endif()
  # The control characters of a string default (written as E'...' escapes, one line) store as the schema's text.
  foreach(db chain snap)
    psql_command(${db} "INSERT INTO svc_sqltest.account (id) VALUES (1);
SELECT 1 / (SELECT count(*)::int FROM svc_sqltest.account WHERE id = 1 AND banner = E'one\\n-- +goose ENVSUB ON\\ttwo\\x01');" rc)
    check_ok(rc "the default of svc_sqltest.account.banner in ${db}")
  endforeach()
  # --- 3. Down returns to v1 (on a second chain that never ran the contract step) -----------------
  psql_command(down "CREATE SCHEMA svc_sqltest;" rc)
  psql_file(down "${WORK_DIR}/v1.up.sql" rc)
  check_ok(rc "v1 migration Up (down)")
  psql_file(down "${WORK_DIR}/v2.up.sql" rc)
  check_ok(rc "v2 migration Up (down)")
  psql_file(down "${WORK_DIR}/v2.down.sql" rc)
  check_ok(rc "v2 migration Down")
  psql_file(snap1 "${v1}/schema.sql" rc)
  check_ok(rc "v1 snapshot")
  dump(down svc_sqltest down_dump)
  dump(snap1 svc_sqltest snap1_dump)
  if(NOT down_dump STREQUAL snap1_dump)
    list(APPEND errors "v2 Down differs from the v1 snapshot (diff ${WORK_DIR}/down.dump ${WORK_DIR}/snap1.dump)")
  endif()
  # --- 4. the committed goldens apply -----------------------------------------------------------
  foreach(snapshot IN LISTS SNAPSHOTS)
    psql_file(golden "${snapshot}" rc)
    check_ok(rc "golden snapshot ${snapshot}")
  endforeach()
endif()

execute_process(COMMAND ${run_as} ${PG_pg_ctl} -D "${data}" -m fast -w stop OUTPUT_QUIET ERROR_QUIET)
if(errors)
  string(REPLACE ";" "\n  " text "${errors}")
  message(FATAL_ERROR "sql_postgres: failed:\n  ${text}")
endif()
execute_process(COMMAND ${run_as} ${PG_psql} --version OUTPUT_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE)
message(STATUS "sql_postgres: passed on ${version}")
