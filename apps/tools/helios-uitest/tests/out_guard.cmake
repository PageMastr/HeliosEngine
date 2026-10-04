# helios-uitest's --out guard (CTest helios_uitest_out_guard). helios-uitest deletes --out at start,
# so it must refuse a directory that is not an earlier run's output, or that holds --goldens or
# --fixture, before it touches a file. Needs no display: every case ends before the editor runs
# (the --editor path does not exist, so an accepted --out ends at "starting ...", exit code 2).
#   cmake -DUITEST=<helios-uitest> -DFIXTURE=<project dir> -DWORK=<scratch dir> -P out_guard.cmake

foreach(v UITEST FIXTURE WORK)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "out_guard.cmake: -D${v}=... is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(GOLDENS "${WORK}/goldens")
file(WRITE "${GOLDENS}/shell_dark_100.png" "not really a png")
file(WRITE "${GOLDENS}/report.json" "{}")
set(NO_EDITOR "${WORK}/no-such-editor")

# uitest(<out dir> <expected stderr text> [<fixture dir>]): exit code 2 with that text on stderr.
function(uitest out needle)
  set(fixture "${FIXTURE}")
  if(ARGC GREATER 2)
    set(fixture "${ARGV2}")
  endif()
  execute_process(COMMAND "${UITEST}" --editor=${NO_EDITOR} --fixture=${fixture} --goldens=${GOLDENS} --out=${out}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  string(FIND "${stderr}" "${needle}" pos)
  if(NOT "${rc}" STREQUAL "2" OR pos EQUAL -1)
    message(FATAL_ERROR "helios-uitest --out=${out} --fixture=${fixture}\n  exit ${rc}, expected 2 with '${needle}'\n"
                        "  stdout: ${stdout}\n  stderr: ${stderr}")
  endif()
  message(STATUS "--out=${out}: ${stderr}")
endfunction()

function(expect_exists path what)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "${what}: ${path} is gone")
  endif()
endfunction()
function(expect_missing path what)
  if(EXISTS "${path}")
    message(FATAL_ERROR "${what}: ${path} is still there")
  endif()
endfunction()

# A directory of the user's: refused, untouched (`--out=.` in a checkout).
file(WRITE "${WORK}/mine/notes.txt" "keep me")
uitest("${WORK}/mine" "is not empty and holds no earlier helios-uitest run")
expect_exists("${WORK}/mine/notes.txt" "a non-empty --out")

# --out is --goldens (which even holds a report.json), or an ancestor of it: refused.
uitest("${GOLDENS}" "would delete --goldens")
uitest("${GOLDENS}/" "would delete --goldens")
uitest("${GOLDENS}/../goldens" "would delete --goldens")
file(WRITE "${WORK}/report.json" "{}")
uitest("${WORK}" "would delete --goldens")
expect_exists("${GOLDENS}/shell_dark_100.png" "--out equal to or above --goldens")

# --out holds the fixture, or lies inside it: refused.
file(COPY "${FIXTURE}/" DESTINATION "${WORK}/fixture")
uitest("${WORK}/fixture" "would delete --fixture" "${WORK}/fixture")
uitest("${WORK}/fixture/out" "lies inside --fixture" "${WORK}/fixture")
expect_missing("${WORK}/fixture/out" "--out inside --fixture")

# A file: refused.
file(WRITE "${WORK}/afile" "x")
uitest("${WORK}/afile" "is not a directory")
expect_exists("${WORK}/afile" "--out naming a file")

# New, empty, or an earlier run's: accepted. The run copies the fixture and leaves the marker, then
# fails to start the (missing) editor.
uitest("${WORK}/run" "starting")
expect_exists("${WORK}/run/helios-uitest-out.txt" "a new --out")
expect_exists("${WORK}/run/project/records/hull/frigate.hrec" "a new --out")
file(WRITE "${WORK}/run/stale.png" "from the earlier run")
uitest("${WORK}/run" "starting")
expect_missing("${WORK}/run/stale.png" "an earlier run's --out")
file(MAKE_DIRECTORY "${WORK}/empty")
uitest("${WORK}/empty" "starting")
file(WRITE "${WORK}/old/report.json" "{}")
file(WRITE "${WORK}/old/grid_dark_100.png" "from a run before the marker")
uitest("${WORK}/old" "starting")
expect_missing("${WORK}/old/grid_dark_100.png" "an --out with report.json")
file(REMOVE_RECURSE "${WORK}")
