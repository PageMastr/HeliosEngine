# helios-cook: the cook CLI (07 §1.10). v0 has the `records` verb (engine/records): `.hrec` →
# records.client.hrdb + records.server.hrdb; and the `check` verb (WP-0.20): the project file and the
# provenance and layout of its content, with engine/assetpipe's `.meta` validation. Links the sample record
# types, like helios-tool. Headless (no graphics module; assetpipe is EDITOR_ONLY, which a tool may link).
helios_executable(helios-cook
  SOURCES main.cpp content_check.cpp
  DEPS helios::records helios::assetpipe helios_toolsfw_samples helios::core)

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

  # The reference project (WP-0.20, 09 §2.1: "Records compile; 100 % provenance"). lint_content_check runs
  # `check` on the repository's helios.project.jsonc and content/; lint_content_records_cook cooks content/'s
  # records; lint_content_records_canonical has helios-tool confirm they are in the canonical form the editor
  # writes; lint_records_sec4_content searches the cook for the server-only aiHints keys of the reference
  # ship (AAA-SEC-4 on the game's own records). The fixture cases copy the project file and content/ into the
  # build tree, seed one violation each (tests/content_check_test.cmake lists them) and must fail with it;
  # the `valid` case copies them unchanged and must pass.
  add_test(NAME lint_content_check COMMAND helios-cook check --project-root=${PROJECT_SOURCE_DIR})
  add_test(NAME lint_content_records_cook
    COMMAND helios-cook records --project-root=${PROJECT_SOURCE_DIR}/content --out=${CMAKE_CURRENT_BINARY_DIR}/content_cook)
  add_test(NAME lint_content_records_canonical
    COMMAND helios-tool --project-root=${PROJECT_SOURCE_DIR}/content --no-journal fmt --check)
  add_test(NAME lint_records_sec4_content
    COMMAND ${CMAKE_COMMAND} -DCOOK=$<TARGET_FILE:helios-cook> -DPROJECT=${PROJECT_SOURCE_DIR}/content
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/sec4_content "-DSENTINELS=breakOffHealth;strafeRange" -P ${_sec4})
  set(_contentCases valid missing_sidecar bad_licence ai_without_record unknown_type hidden_file undeclared_zone
      spatial_outside_zone global_in_zone container_group container_identity entity_name project_unknown_key
      project_tick_rate project_reserved_scheme broken_record)
  set(_contentTests lint_content_check lint_content_records_cook lint_content_records_canonical lint_records_sec4_content)
  foreach(case IN LISTS _contentCases)
    add_test(NAME lint_content_check_fixture_${case}
      COMMAND ${CMAKE_COMMAND} -DCOOK=$<TARGET_FILE:helios-cook> -DSOURCE=${PROJECT_SOURCE_DIR}
              -DWORK=${CMAKE_CURRENT_BINARY_DIR}/content_fixture/${case} -DCASE=${case}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/content_check_test.cmake)
    list(APPEND _contentTests lint_content_check_fixture_${case})
  endforeach()
  set_tests_properties(${_contentTests} PROPERTIES LABELS lint TIMEOUT 120)
  unset(_contentCases)
  unset(_contentTests)
  unset(_sec4)
endif()
