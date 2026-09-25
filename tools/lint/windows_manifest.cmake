# Windows manifest lint (ADR-011, 02 §2.1, WP-0.5).
#
#   cmake -DMANIFEST=<helios.manifest> [-DBINARY=<built .exe>] -P windows_manifest.cmake
#
# Checks that the manifest sets the UTF-8 active code page, PerMonitorV2 DPI awareness,
# longPathAware, the Windows 10/11 supportedOS GUID and asInvoker, and (with BINARY) that a built
# Windows executable really embeds those settings (the manifest is stored as plain UTF-8 XML in the
# RT_MANIFEST resource, so a byte search is exact). Runs on any host, including the Linux host of the
# MinGW cross build.

cmake_minimum_required(VERSION 3.28)
if(NOT MANIFEST OR NOT EXISTS "${MANIFEST}")
  message(FATAL_ERROR "windows_manifest: MANIFEST not found: '${MANIFEST}'")
endif()
file(READ "${MANIFEST}" xml)
set(findings "")
set(required
  "<activeCodePage[^>]*>UTF-8</activeCodePage>|activeCodePage UTF-8 (process ANSI code page is UTF-8)"
  "<dpiAwareness[^>]*>PerMonitorV2</dpiAwareness>|dpiAwareness PerMonitorV2"
  "<longPathAware[^>]*>true</longPathAware>|longPathAware true"
  "<supportedOS Id=\"\\{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a\\}\"/>|supportedOS Windows 10/11"
  "<requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\"/>|requestedExecutionLevel asInvoker"
  "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">|assembly root element")
foreach(r IN LISTS required)
  string(FIND "${r}" "|" bar)
  string(SUBSTRING "${r}" 0 ${bar} pattern)
  math(EXPR start "${bar} + 1")
  string(SUBSTRING "${r}" ${start} -1 what)
  if(NOT xml MATCHES "${pattern}")
    list(APPEND findings "${MANIFEST}: missing ${what}")
  endif()
endforeach()

if(BINARY)
  if(NOT EXISTS "${BINARY}")
    list(APPEND findings "${BINARY}: not built")
  else()
    foreach(needle "<activeCodePage" "PerMonitorV2" "<longPathAware")
      file(STRINGS "${BINARY}" hits REGEX "${needle}" LIMIT_COUNT 1)
      if(NOT hits)
        list(APPEND findings "${BINARY}: embedded manifest lacks '${needle}' (helios_windows_manifest not applied?)")
      endif()
    endforeach()
  endif()
endif()

if(findings)
  string(REPLACE ";" "\n  " text "${findings}")
  message(FATAL_ERROR "Windows manifest check failed:\n  ${text}\n")
endif()
if(BINARY)
  message(STATUS "Windows manifest check passed (embedded in ${BINARY})")
else()
  message(STATUS "Windows manifest check passed")
endif()
