# helios-tool CLI round trip (CTest helios_tool_cli). Runs with cmake -P so it works the same on
# Windows and Linux:
#   cmake -DTOOL=<helios-tool> -DFIXTURE=<project dir> -DWORK=<scratch dir> -P cli_test.cmake
# Every step checks the exit code and, where it matters, the record file's bytes.

foreach(v TOOL FIXTURE WORK)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "cli_test.cmake: -D${v}=... is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(COPY "${FIXTURE}/" DESTINATION "${WORK}/project")
set(PROJECT_DIR "${WORK}/project")
set(JOURNALS "${WORK}/journal")
set(FRIGATE "${PROJECT_DIR}/records/hull/frigate.hrec")
file(READ "${FRIGATE}" ORIGINAL)

# Control characters for the journal-escaping steps below.
string(ASCII 27 ESC)
string(ASCII 7 BEL)
string(ASCII 127 DEL)
string(ASCII 194 155 CSI1)  # U+009B in UTF-8

set(step 0)
# tool(<expected exit code> <output var> args...)
function(tool expected outvar)
  math(EXPR n "${step} + 1")
  set(step ${n} PARENT_SCOPE)
  execute_process(COMMAND "${TOOL}" --project-root=${PROJECT_DIR} --project=cli-test --journal-dir=${JOURNALS} ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  # Child stdout is text mode on Windows (CRLF).
  string(REPLACE "\r\n" "\n" out "${out}")
  # This log shows the arguments without their control characters (the hostile-journal steps).
  set(args "${ARGN}")
  foreach(c ESC BEL DEL CSI1)
    string(REPLACE "${${c}}" "<${c}>" args "${args}")
  endforeach()
  if(NOT "${rc}" STREQUAL "${expected}")
    message(FATAL_ERROR "step ${n}: helios-tool ${args}\n  exit ${rc}, expected ${expected}\n  stdout: ${out}\n  stderr: ${err}")
  endif()
  message(STATUS "step ${n}: helios-tool ${args} -> ${rc}")
  set(${outvar} "${out}" PARENT_SCOPE)
  set(${outvar}_err "${err}" PARENT_SCOPE)
endfunction()

function(expect_contains text needle what)
  string(FIND "${text}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "${what}: '${needle}' not found in:\n${text}")
  endif()
endfunction()

function(expect_file_equals file expected what)
  file(READ "${file}" actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "${what}: ${file} differs:\n${actual}")
  endif()
endfunction()

# Read-only verbs.
tool(0 out --version)
tool(2 out)
tool(2 out no-such-verb)
tool(0 out validate)
expect_contains("${out}" "0 error(s)" "validate")
tool(0 out validate --json)
expect_contains("${out}" "\"errors\":0" "validate --json")
tool(0 out fmt --check)
tool(0 out commands)
expect_contains("${out}" "doc.setProperty" "commands")
tool(0 out commands --json)
expect_contains("${out}" "\"id\":\"validate.run\"" "commands --json")

# apply -> undo -> redo -> undo, each in its own process, through the journal.
tool(0 out apply hull/frigate mass 13000)
expect_contains("${out}" "saved 1 file(s)" "apply")
file(READ "${FRIGATE}" after)
expect_contains("${after}" "\"mass\": 13000," "apply wrote the file")
tool(0 out apply hull/frigate handling/yawRate 33.5)
tool(0 out undo)
expect_contains("${out}" "undid" "undo")
file(READ "${FRIGATE}" t)
expect_contains("${t}" "\"yawRate\": 30," "undo restored yawRate")
expect_contains("${t}" "\"mass\": 13000," "undo kept mass")
tool(0 out undo)
expect_file_equals("${FRIGATE}" "${ORIGINAL}" "two undos restore the fixture byte for byte")
tool(3 out undo)
expect_contains("${out}" "" "nothing to undo")
tool(0 out redo --steps=2)
file(READ "${FRIGATE}" t)
expect_contains("${t}" "\"yawRate\": 33.5," "redo re-applied yawRate")
expect_contains("${t}" "\"mass\": 13000," "redo re-applied mass")
tool(0 out undo --steps=2)
expect_file_equals("${FRIGATE}" "${ORIGINAL}" "undo --steps=2 restores the fixture")

# A conflicting external edit blocks the undo instead of clobbering it.
tool(0 out apply hull/frigate mass 14000)
file(READ "${FRIGATE}" t)
string(REPLACE "\"mass\": 14000," "\"mass\": 15000," t "${t}")
file(WRITE "${FRIGATE}" "${t}")
tool(3 out undo)
file(READ "${FRIGATE}" t)
expect_contains("${t}" "\"mass\": 15000," "a conflicting undo leaves the file alone")
file(WRITE "${FRIGATE}" "${ORIGINAL}")

# Invalid edits are refused (schema @range / types) and change nothing.
tool(3 out apply hull/frigate mass "\"heavy\"")
tool(3 out apply hull/frigate no/such/path 1)
expect_file_equals("${FRIGATE}" "${ORIGINAL}" "refused edits change nothing")

# Several commands in one transaction.
file(WRITE "${WORK}/batch.jsonl"
  "// two edits, one undo step\n"
  "{\"command\": \"doc.setProperty\", \"args\": {\"doc\": \"hull/frigate\", \"path\": \"mass\", \"value\": 12500}}\n"
  "{\"command\": \"doc.setProperty\", \"args\": {\"doc\": \"hull/frigate\", \"path\": \"handling/rollRate\", \"value\": 95}}\n")
tool(0 out apply --file=${WORK}/batch.jsonl)
expect_contains("${out}" "(2 op(s))" "batch is one transaction")
tool(0 out undo)
expect_file_equals("${FRIGATE}" "${ORIGINAL}" "one undo reverts the batch")

# A batch is one transaction group: a save of a document the batch has edited is refused, and
# nothing is written (the save would reach the journal before the batch's transaction).
file(WRITE "${WORK}/batch_save.jsonl"
  "{\"command\": \"doc.setProperty\", \"args\": {\"doc\": \"hull/frigate\", \"path\": \"mass\", \"value\": 12500}}\n"
  "{\"command\": \"doc.save\", \"args\": {\"doc\": \"hull/frigate\"}}\n")
tool(3 out apply --file=${WORK}/batch_save.jsonl)
expect_file_equals("${FRIGATE}" "${ORIGINAL}" "a batch cannot save a document it edited")

# A journal-only edit (--no-save) is not on the CLI undo stack: undo skips it and reverts the
# last saved edit, and every run's transaction ids are distinct (the Lamport counter continues
# across runs).
tool(0 out apply hull/frigate mass 13000)
tool(0 out apply hull/frigate mass 12345 --no-save)
expect_file_equals("${FRIGATE}" "${after}" "--no-save leaves the file alone")
tool(0 out undo)
expect_file_equals("${FRIGATE}" "${ORIGINAL}" "undo skips the journal-only edit")
if(NOT out MATCHES "undid ([^ ]+) -> ([^ \n]+)")
  message(FATAL_ERROR "undo output: ${out}")
endif()
if(CMAKE_MATCH_1 STREQUAL CMAKE_MATCH_2)
  message(FATAL_ERROR "the undo reused the id of its target: ${out}")
endif()

# Journal-only edit (--no-save), then replay it from the journal.
tool(0 out apply hull/frigate mass 12345 --no-save)
expect_file_equals("${FRIGATE}" "${ORIGINAL}" "--no-save leaves the file alone")
tool(0 out journal list --all)
expect_contains("${out}" "session=" "journal list")
tool(0 out journal show latest)
expect_contains("${out}" "tx " "journal show")
if(NOT out MATCHES "session=([^ \n]+)")
  message(FATAL_ERROR "journal show: no session in ${out}")
endif()
set(nosave_journal "${JOURNALS}/cli-test/${CMAKE_MATCH_1}.hjl")
tool(0 out journal verify latest)
expect_contains("${out}" "0 torn byte(s)" "journal verify")
tool(0 out journal replay ${nosave_journal} --save)
expect_contains("${out}" "replayed 1 transaction(s)" "journal replay")
file(READ "${FRIGATE}" t)
expect_contains("${t}" "\"mass\": 12345," "the replayed journal-only edit reached the file")
tool(0 out undo)
expect_file_equals("${FRIGATE}" "${ORIGINAL}" "the replayed (and saved) edit is undoable")

# The journal is untrusted input. A journal whose header names another project is refused and
# nothing is written, unless --allow-other-project says the project was renamed. (Crafted paths
# are covered by toolsfw_tests, test_confine.cpp; the CLI's replay is Framework::recover.)
tool(3 out --project=renamed-cli-test journal replay ${nosave_journal} --save)
expect_contains("${out_err}" "belongs to project \"cli-test\", not \"renamed-cli-test\"" "replay of another project's journal")
expect_file_equals("${FRIGATE}" "${ORIGINAL}" "a refused replay writes nothing")
tool(0 out --project=renamed-cli-test journal replay ${nosave_journal} --save --allow-other-project)
expect_contains("${out}" "replayed 1 transaction(s)" "journal replay --allow-other-project")
file(READ "${FRIGATE}" t)
expect_contains("${t}" "\"mass\": 12345," "the allowed replay reached the file")
file(WRITE "${FRIGATE}" "${ORIGINAL}")

# Journal strings are untrusted input, and the CLI never writes their control characters to the
# terminal. --user and --project put them into real journals: ESC [ 31 m sets the colour, then
# BEL, DEL, and U+009B 2 J (the C1 CSI, which JSON's escaping of C0 only lets through) clears the
# screen. The closing ']' balances the '[' (CMake does not split a list inside brackets), and
# there is no ';' (CMake's list separator), so no OSC sequence here; test_confine.cpp has those.
set(EVIL "${ESC}[31m${BEL}${DEL}${CSI1}2J]")
set(EVIL_SHOWN "\\x1b[31m\\x07\\x7f\\u009b2J]")
function(expect_no_controls text what)
  foreach(c "${ESC}" "${BEL}" "${DEL}" "${CSI1}")
    string(FIND "${text}" "${c}" pos)
    if(NOT pos EQUAL -1)
      message(FATAL_ERROR "${what}: a raw control character reached the output")
    endif()
  endforeach()
endfunction()
tool(0 out "--user=${EVIL}" apply hull/frigate mass 12346 --no-save)
expect_no_controls("${out}" "apply with a hostile --user")
tool(0 out journal show latest)
expect_no_controls("${out}" "journal show")
expect_contains("${out}" "user=${EVIL_SHOWN}" "journal show escapes the header")
if(NOT out MATCHES "session=([^ \n]+)")
  message(FATAL_ERROR "journal show: no session in ${out}")
endif()
set(evil_journal "${JOURNALS}/cli-test/${CMAKE_MATCH_1}.hjl")
tool(0 out journal show latest --json)
expect_no_controls("${out}" "journal show --json")
expect_contains("${out}" "\"user\":\"\\u001b[31m\\u0007\\u007f\\u009b2J]\"" "journal show --json escapes DEL and C1")
# The replay report: an external edit makes the transaction conflict, and the report names it.
string(REGEX REPLACE "\"mass\": [0-9]+," "\"mass\": 15000," t "${ORIGINAL}")
file(WRITE "${FRIGATE}" "${t}")
tool(3 out journal replay ${evil_journal} --ignore-source-changes)
expect_no_controls("${out}${out_err}" "journal replay report")
expect_contains("${out}" "conflict transaction ${EVIL_SHOWN}:" "the replay report escapes the transaction id")
file(WRITE "${FRIGATE}" "${ORIGINAL}")
# A refusal: the header names a project with the same characters.
tool(0 out "--project=${EVIL}" apply hull/frigate mass 12347 --no-save)
tool(0 out "--project=${EVIL}" journal list --all)
expect_no_controls("${out}" "journal list")
if(NOT out MATCHES "([^\n]+\\.hjl)  session=")
  message(FATAL_ERROR "journal list: no journal in ${out}")
endif()
set(evil_project_journal "${CMAKE_MATCH_1}")
tool(3 out journal replay ${evil_project_journal} --save)
expect_no_controls("${out}${out_err}" "a refused replay")
expect_contains("${out_err}" "belongs to project \"${EVIL_SHOWN}\", not \"cli-test\"" "the refusal escapes the project")
expect_file_equals("${FRIGATE}" "${ORIGINAL}" "a refused replay writes nothing")

# A command may not create a record outside the project or of another file type.
tool(3 out apply --command=doc.create
     "--args={\"type\": \"sample.ship.ShipHullDef\", \"file\": \"../outside/evil.hrec\", \"name\": \"hull/evil\"}")
expect_contains("${out_err}" "../outside/evil.hrec" "doc.create outside the project")
tool(3 out apply --command=doc.create
     "--args={\"type\": \"sample.ship.ShipHullDef\", \"file\": \"records/hull/evil.sh\", \"name\": \"hull/evil\"}")
if(EXISTS "${WORK}/outside" OR EXISTS "${PROJECT_DIR}/records/hull/evil.sh")
  message(FATAL_ERROR "a refused doc.create wrote a file")
endif()
file(GLOB journals "${JOURNALS}/*/*.hjl")
list(LENGTH journals njournals)
if(njournals LESS 10)
  message(FATAL_ERROR "expected one journal per editing run, found ${njournals}")
endif()

# fmt: a valid but non-canonical file (CRLF endings, extra spaces) is reported, then rewritten.
file(READ "${PROJECT_DIR}/records/itm/scrap_plate.hrec" item)
string(REPLACE "\n" "\r\n" messy "${item}")
string(REPLACE "\"stackMax\": 50" "\"stackMax\":    50" messy "${messy}")
file(WRITE "${PROJECT_DIR}/records/itm/scrap_plate.hrec" "${messy}")
tool(1 out fmt --check)
expect_contains("${out}" "not canonical: records/itm/scrap_plate.hrec" "fmt --check")
tool(0 out fmt)
expect_file_equals("${PROJECT_DIR}/records/itm/scrap_plate.hrec" "${item}" "fmt restores the canonical text")
tool(0 out fmt --check)

# Automation script (Luau).
file(WRITE "${WORK}/script.luau" "
local before = Record.get('hull/frigate', 'mass')
Editor.transaction('Script edit', function()
  Record.set('hull/frigate', 'mass', before + 1)
end)
print('mass', Record.get('hull/frigate', 'mass'))
")
tool(0 out run ${WORK}/script.luau --no-save)
expect_contains("${out}" "mass" "run prints")
expect_file_equals("${FRIGATE}" "${ORIGINAL}" "run --no-save leaves the file alone")

file(REMOVE_RECURSE "${WORK}")
message(STATUS "helios-tool CLI: ${step} steps passed")
