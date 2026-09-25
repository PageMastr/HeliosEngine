# Slang shader compiler bootstrap (ADR-003): downloads the pinned prebuilt Slang release for the
# host, verifies its SHA-256 and unpacks it to tools/prebuilt/slang/<version>/<platform>/.
#
# Two ways to use it:
#   * Script mode (manual bootstrap / CI cache warm-up):
#         cmake -P tools/prebuilt/fetch_slang.cmake
#     prints the Slang root directory on success.
#   * From CMake (cmake/HeliosShaders.cmake does this at configure time):
#         include(${CMAKE_SOURCE_DIR}/tools/prebuilt/fetch_slang.cmake)
#         helios_fetch_slang(<out-root-var>)
#
# HELIOS_SLANG_ROOT (CMake cache variable or environment variable) overrides the download: it must
# point at an unpacked Slang release (the directory containing bin/slangc[.exe]). Use it for
# offline machines, unsupported hosts (arm64) or when testing another Slang build.
#
# Archives are cached in tools/prebuilt/cache/ and unpacked trees in tools/prebuilt/slang/; both
# are gitignored. A stamp file records the verified archive hash, so a changed pin re-extracts.
# Concurrent configures (several build directories at once) are serialized with a file lock.

cmake_minimum_required(VERSION 3.28)

set(HELIOS_SLANG_VERSION "2026.18.2")
set(HELIOS_SLANG_BASE_URL "https://github.com/shader-slang/slang/releases/download/v${HELIOS_SLANG_VERSION}")

# Pinned assets. Hashes were computed from the published release assets (v2026.18.2, 2026-09-22);
# a mismatch fails the download, so a re-published or tampered asset can never be used silently.
set(HELIOS_SLANG_ASSET_linux-x86_64 "slang-${HELIOS_SLANG_VERSION}-linux-x86_64.tar.gz")
set(HELIOS_SLANG_SHA256_linux-x86_64 "8a097d4365e1cab10265d0b0d77b461b5d30576f99ddc0521a35723c34cad816")
set(HELIOS_SLANG_ASSET_windows-x86_64 "slang-${HELIOS_SLANG_VERSION}-windows-x86_64.zip")
set(HELIOS_SLANG_SHA256_windows-x86_64 "747602aec6b3623658d55fea87492d71828e15d16802d7941204fde418ceee8e")

get_filename_component(HELIOS_PREBUILT_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

# helios_slang_host_platform(<out-var>): "linux-x86_64", "windows-x86_64" or "" (unsupported).
function(helios_slang_host_platform out_var)
  set(platform "")
  set(cpu "${CMAKE_HOST_SYSTEM_PROCESSOR}")
  if(NOT cpu)  # not set in script mode (cmake -P)
    cmake_host_system_information(RESULT cpu QUERY OS_PLATFORM)
  endif()
  string(TOLOWER "${cpu}" cpu)
  if(cpu MATCHES "^(x86_64|amd64|x64)$")
    if(CMAKE_HOST_WIN32)
      set(platform "windows-x86_64")
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
      set(platform "linux-x86_64")
    endif()
  endif()
  set(${out_var} "${platform}" PARENT_SCOPE)
endfunction()

# helios_slang_compiler_path(<root> <out-var>): path of slangc inside an unpacked release.
function(helios_slang_compiler_path root out_var)
  if(CMAKE_HOST_WIN32)
    set(${out_var} "${root}/bin/slangc.exe" PARENT_SCOPE)
  else()
    set(${out_var} "${root}/bin/slangc" PARENT_SCOPE)
  endif()
endfunction()

# helios_fetch_slang(<out-root-var>): sets <out-root-var> to the Slang root directory, downloading
# and verifying the pinned release when needed. Fails the configure step with instructions if
# neither an override nor a verified download is available.
function(helios_fetch_slang out_var)
  # 1. Explicit override.
  set(override "${HELIOS_SLANG_ROOT}")
  if(NOT override AND DEFINED ENV{HELIOS_SLANG_ROOT})
    set(override "$ENV{HELIOS_SLANG_ROOT}")
  endif()
  if(override)
    file(TO_CMAKE_PATH "${override}" override)
    helios_slang_compiler_path("${override}" slangc)
    if(NOT EXISTS "${slangc}")
      message(FATAL_ERROR "HELIOS_SLANG_ROOT='${override}' does not contain ${slangc}")
    endif()
    set(${out_var} "${override}" PARENT_SCOPE)
    return()
  endif()

  # 2. Pinned download for the host.
  helios_slang_host_platform(platform)
  if(NOT platform)
    message(FATAL_ERROR
      "No pinned Slang ${HELIOS_SLANG_VERSION} release for host ${CMAKE_HOST_SYSTEM_NAME}/"
      "${CMAKE_HOST_SYSTEM_PROCESSOR}. Unpack a Slang release and set HELIOS_SLANG_ROOT to it.")
  endif()
  set(asset "${HELIOS_SLANG_ASSET_${platform}}")
  set(sha256 "${HELIOS_SLANG_SHA256_${platform}}")
  string(LENGTH "${sha256}" sha256_length)
  if(NOT sha256 MATCHES "^[0-9a-f]+$" OR NOT sha256_length EQUAL 64)
    # Guard for future platforms added without a verified pin: never extract an unverified binary.
    message(FATAL_ERROR "Slang asset ${asset} has no verified SHA-256 pin in ${CMAKE_CURRENT_FUNCTION_LIST_FILE}; "
                        "set HELIOS_SLANG_ROOT to a trusted Slang ${HELIOS_SLANG_VERSION} release instead.")
  endif()

  set(root "${HELIOS_PREBUILT_DIR}/slang/${HELIOS_SLANG_VERSION}/${platform}")
  set(stamp "${root}/.helios-slang-stamp")
  helios_slang_compiler_path("${root}" slangc)

  set(stamp_contents "")
  if(EXISTS "${stamp}")
    file(READ "${stamp}" stamp_contents)
    string(STRIP "${stamp_contents}" stamp_contents)
  endif()
  if(stamp_contents STREQUAL sha256 AND EXISTS "${slangc}")
    set(${out_var} "${root}" PARENT_SCOPE)
    return()
  endif()

  file(MAKE_DIRECTORY "${HELIOS_PREBUILT_DIR}/cache")
  file(LOCK "${HELIOS_PREBUILT_DIR}/cache/slang.lock" GUARD FUNCTION TIMEOUT 900 RESULT_VARIABLE lock_result)
  if(NOT lock_result EQUAL 0)
    message(FATAL_ERROR "Could not lock ${HELIOS_PREBUILT_DIR}/cache/slang.lock: ${lock_result}")
  endif()
  # Another configure may have finished the job while we waited for the lock.
  if(EXISTS "${stamp}")
    file(READ "${stamp}" stamp_contents)
    string(STRIP "${stamp_contents}" stamp_contents)
    if(stamp_contents STREQUAL sha256 AND EXISTS "${slangc}")
      set(${out_var} "${root}" PARENT_SCOPE)
      return()
    endif()
  endif()

  set(archive "${HELIOS_PREBUILT_DIR}/cache/${asset}")
  set(have_archive FALSE)
  if(EXISTS "${archive}")
    file(SHA256 "${archive}" existing_hash)
    if(existing_hash STREQUAL sha256)
      set(have_archive TRUE)
    else()
      message(STATUS "Slang: cached ${asset} has hash ${existing_hash}; downloading again")
      file(REMOVE "${archive}")
    endif()
  endif()
  if(NOT have_archive)
    message(STATUS "Slang: downloading ${HELIOS_SLANG_BASE_URL}/${asset}")
    set(partial "${archive}.part")
    file(DOWNLOAD "${HELIOS_SLANG_BASE_URL}/${asset}" "${partial}"
         EXPECTED_HASH SHA256=${sha256}
         TLS_VERIFY ON
         INACTIVITY_TIMEOUT 120
         STATUS download_status)
    list(GET download_status 0 download_code)
    if(NOT download_code EQUAL 0)
      list(GET download_status 1 download_message)
      file(REMOVE "${partial}")
      message(FATAL_ERROR
        "Slang download failed (${download_code}: ${download_message}). Either allow access to "
        "github.com release assets or unpack ${asset} yourself and set HELIOS_SLANG_ROOT.")
    endif()
    file(RENAME "${partial}" "${archive}")
  endif()

  # Extract into a scratch directory first so an interrupted extraction never looks complete.
  set(scratch "${root}.extracting")
  file(REMOVE_RECURSE "${scratch}" "${root}")
  file(MAKE_DIRECTORY "${scratch}")
  file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${scratch}")
  # Tolerate releases that wrap everything in one top-level directory.
  set(scratch_inner "")
  if(NOT EXISTS "${scratch}/bin")
    file(GLOB entries "${scratch}/*")
    list(LENGTH entries entry_count)
    if(entry_count EQUAL 1 AND IS_DIRECTORY "${entries}")
      set(scratch_inner "${entries}")
    endif()
  endif()
  if(scratch_inner)
    file(RENAME "${scratch_inner}" "${root}")
    file(REMOVE_RECURSE "${scratch}")
  else()
    file(RENAME "${scratch}" "${root}")
  endif()
  if(NOT EXISTS "${slangc}")
    message(FATAL_ERROR "Slang archive ${asset} did not contain ${slangc}")
  endif()
  file(WRITE "${stamp}" "${sha256}\n")
  message(STATUS "Slang ${HELIOS_SLANG_VERSION} (${platform}) unpacked to ${root}")
  set(${out_var} "${root}" PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  helios_fetch_slang(slang_root)
  message("${slang_root}")
endif()
