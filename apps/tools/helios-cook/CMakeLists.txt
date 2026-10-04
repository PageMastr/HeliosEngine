# helios-cook: the cook CLI (07 §1.10). v0 has the `records` verb (engine/records): `.hrec` →
# records.client.hrdb + records.server.hrdb. Links the sample record types, like helios-tool. Headless.
helios_executable(helios-cook
  SOURCES main.cpp
  DEPS helios::records helios_toolsfw_samples helios::core)

# AAA-SEC-4 over the sample records (lint): cook engine/records/samples/project, whose server-only fields
# and server-only loot table carry planted sentinels, and the M0 sample project of engine/toolsfw; then
# search the cooked bytes: each sentinel must be in the server cook (so the search can find it) and in no
# client cook. The fixture test scans the server cook as if it were the client's and must fail.
if(HELIOS_BUILD_TESTS)
  set(_sec4 ${PROJECT_SOURCE_DIR}/engine/records/tests/sec4_check.cmake)
  add_test(NAME lint_records_sec4
    COMMAND ${CMAKE_COMMAND} -DCOOK=$<TARGET_FILE:helios-cook> -DPROJECT=${PROJECT_SOURCE_DIR}/engine/records/samples/project
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/sec4_samples
            "-DSENTINELS=Sec4Sentinel;Sec4SentinelAiHint;Sec4SentinelLootItem;SEC4-SENTINEL-salvage-note" -P ${_sec4})
  add_test(NAME lint_records_sec4_m0_sample
    COMMAND ${CMAKE_COMMAND} -DCOOK=$<TARGET_FILE:helios-cook> -DPROJECT=${PROJECT_SOURCE_DIR}/engine/toolsfw/samples/project
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/sec4_m0 "-DSENTINELS=preferredRange;retreatHealth" -P ${_sec4})
  add_test(NAME lint_records_sec4_fixture_leak
    COMMAND ${CMAKE_COMMAND} "-DEXPECT=AAA-SEC-4: server-only sentinel 'Sec4Sentinel' is in the client cook" -DNARGS=8
            -DARG0=${CMAKE_COMMAND} -DARG1=-DCOOK=$<TARGET_FILE:helios-cook>
            -DARG2=-DPROJECT=${PROJECT_SOURCE_DIR}/engine/records/samples/project
            -DARG3=-DWORK=${CMAKE_CURRENT_BINARY_DIR}/sec4_fixture -DARG4=-DSENTINELS=Sec4Sentinel -DARG5=-DSWAP=ON
            -DARG6=-P -DARG7=${_sec4}
            -P ${PROJECT_SOURCE_DIR}/tools/lint/expect_fail.cmake)
  set_tests_properties(lint_records_sec4 lint_records_sec4_m0_sample lint_records_sec4_fixture_leak PROPERTIES LABELS lint TIMEOUT 120)
  unset(_sec4)
endif()
