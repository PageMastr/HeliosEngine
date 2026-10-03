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

set(step 0)
# tool(<expected exit code> <output var> args...)
function(tool expected outvar)
  math(EXPR n "${step} + 1")
  set(step ${n} PARENT_SCOPE)
  execute_process(COMMAND "${TOOL}" --project-root=${PROJECT_DIR} --project=cli-test --journal-dir=${JOURNALS} ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  # Child stdout is text mode on Windows (CRLF).
  string(REPLACE "\r\n" "\n" out "${out}")
  if(NOT "${rc}" STREQUAL "${expected}")
    message(FATAL_ERROR "step ${n}: helios-tool ${ARGN}\n  exit ${rc}, expected ${expected}\n  stdout: ${out}\n  stderr: ${err}")
  endif()
  message(STATUS "step ${n}: helios-tool ${ARGN} -> ${rc}")
  set(${outvar} "${out}" PARENT_SCOPE)
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
