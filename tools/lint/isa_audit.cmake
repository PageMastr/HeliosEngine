# ISA audit (02 §1.1 "The audit"; WP-0.2, reworked to whole-image levels by WP-0.2r; RT-09 AVX2 part). The
# CMake driver of 02 §1.1's five checks until `helios-tool isa-audit` takes them over (WP-0.18). It runs as
# a CMake script, so it works on every developer machine without Python:
#
#   cmake -DCOMPILE_COMMANDS=<build>/compile_commands.json -DLEVELS=<build>/helios_generated/isa_levels.txt
#         -DALLOWLIST=<src>/cmake/isa_allowlist.cmake [-DOBJDUMP=objdump] [-DNM=nm] [-DREADELF=readelf]
#         [-DIMAGES_FILE=<gated executables>] [-DSHARED_IMAGES_FILE=<shared libraries they load>]
#         [-DBASE_IMAGES_FILE=<base executables>] [-DSKIP_OBJECTS=ON] [-DREQUIRE_GATE=ON] -P isa_audit.cmake
#   cmake -DMODE=object     -DOBJECT=<file.o>  -DALLOWLIST=... -DOBJDUMP=... -DNM=... -P isa_audit.cmake
#   cmake -DMODE=image      -DIMAGE=<exe>      -DALLOWLIST=... -DREADELF=... -DNM=... -P isa_audit.cmake
#   cmake -DMODE=shared_image -DIMAGE=<lib.so> -DALLOWLIST=... -DREADELF=... -P isa_audit.cmake
#   cmake -DMODE=base_image -DIMAGE=<exe>      -DALLOWLIST=... -DOBJDUMP=... -P isa_audit.cmake
#   cmake -DMODE=base_sources -DSOURCE_DIRS=<dir>[|<dir>...] -DALLOWLIST=... -P isa_audit.cmake
#
# LEVELS is written at configure time by helios_isa_finalize() (cmake/HeliosIsa.cmake): one
# "<target> <level>" line per target, the level being avx2, base or gate. A violation fails the run and is
# printed with the unit, object or image it concerns.
#  1. Flags, per translation unit of compile_commands.json, against its target's level:
#     - every unit's target has a level;
#     - avx2 units carry the whole avx2 set (GCC/Clang: AVX2, BMI1/2, LZCNT, POPCNT, F16C and
#       -ffp-contract=off; clang-cl the same through /arch:AVX2 and /clang:-ffp-contract=off; MSVC
#       /arch:AVX2), and nothing above it: no FMA, no AVX-512, no other extension (-maes, -msha, an -march
#       beyond x86-64, ...);
#     - base and gate units carry no flag above x86-64-v1 (-msse3…-msse4.2, -mpopcnt, -mlzcnt, -mcx16,
#       -mavx*, -mbmi*, -mfma, an -march other than x86-64, an /arch other than SSE2), forwarded
#       /clang: options included, and carry the base set itself: with GCC, Clang and MinGW an -march
#       (x86-64) and -mtune=generic, because their default -march is the toolchain's (x86-64-v2 on
#       RHEL 9, for one); x64 MSVC and clang-cl have no base flags;
#     - gate units also end with the stack protector off (-fno-stack-protector, or /GS- with cl and
#       clang-cl), no sanitizer still enabled (-fno-sanitize=all after any -fsanitize=), no MSVC run-time
#       checks (/RTC*) and no coverage instrumentation (--coverage, which nothing negates; -fprofile-arcs,
#       -fprofile-instr-generate, -f[cs-]profile-generate and -fcoverage-mapping unless their own -fno-
#       option follows; -fsanitize-coverage=, which no -fno-sanitize-coverage= clears here);
#     - the CPU gate's sources are compiled only in gate-level targets;
#     - no unit uses -march=native, fast-math or FP contraction (determinism across compilers, 02 §7.1).
#  2. The gate objects (GNU binutils): no VEX/EVEX, BMI, LZCNT/TZCNT, POPCNT, MOVBE, CMPXCHG16B or SSE3+
#     instruction; no weak, COMDAT/unique or IFUNC symbol; only the listed exports; every undefined
#     symbol on HELIOS_ISA_GATE_ALLOWED_IMPORTS, so no __stack_chk_*, __security_cookie or __asan_*.
#  3. (ELF part, until WP-0.2r part 2 adds the rest) Gated ELF executables: .preinit_array holds
#     exactly one entry, the gate, and no R_X86_64_IRELATIVE relocation exists. The shared libraries a
#     gated executable loads at start-up (in a modular dev build the link-group libraries, SDL3 and
#     tp_imgui: SHARED_IMAGES_FILE) have no R_X86_64_IRELATIVE relocation either, because the dynamic
#     linker relocates them before it runs .preinit_array (ADR-0.6c §3 item 6).
#  4. Base images after linking (GNU binutils): every instruction above x86-64-v1 is mapped to its
#     symbol, and every such symbol must be on HELIOS_ISA_SELF_DISPATCH_SYMBOLS. TZCNT's encoding is
#     accepted as BSF: GCC and Clang emit `rep bsf` for count-trailing-zeros at x86-64-v1 (see below),
#     which leaves a false negative that MODE=base_sources narrows for Helios's own base modules.
#     MODE=base_sources: the Helios base modules' sources hold no target attribute or pragma that grants
#     BMI, and no TZCNT intrinsic or TZCNT in inline assembly (a text scan; see _isa_scan_base_sources).
# MSVC and clang-cl: check 1 runs on the Ninja generators' compile_commands.json. Checks 2 to 4 on COFF
# images and PDBs (dumpbin, llvm-pdbutil) are `helios-tool isa-audit`'s, WP-0.2r part 2; until then
# tools/ci/msvc_gate_audit.ps1 can run check 2 with dumpbin by hand (no CI job calls it).

cmake_minimum_required(VERSION 3.28)

if(NOT ALLOWLIST OR NOT EXISTS "${ALLOWLIST}")
  message(FATAL_ERROR "isa_audit: ALLOWLIST (cmake/isa_allowlist.cmake) not given or missing: '${ALLOWLIST}'")
endif()
include("${ALLOWLIST}")
# The attributed pre-main hooks live next to the allowlist (the ELF gate hook's symbol is one).
get_filename_component(_isaListDir "${ALLOWLIST}" DIRECTORY)
if(EXISTS "${_isaListDir}/pre_main_allowlist.cmake")
  include("${_isaListDir}/pre_main_allowlist.cmake")
endif()

set(violations "")
macro(_violation text)
  list(APPEND violations "${text}")
endmacro()

# ---------------------------------------------------------------------------------------------
# Flag evaluation
# ---------------------------------------------------------------------------------------------
# AVX-class features, in the order diagnostics list them. popcnt (x86-64-v2) is tracked separately.
set(_ISA_FEATURES avx avx2 fma bmi bmi2 f16c lzcnt avx512)
# What an avx2 unit must enable: 02 §1.1's avx2 set.
set(_ISA_AVX2_REQUIRED avx2 bmi bmi2 lzcnt popcnt f16c)
# -m extensions an avx2 unit may name: the set itself and what AVX2 hardware implies below it.
set(_ISA_AVX2_NAMED avx2 avx bmi bmi2 lzcnt popcnt f16c sse sse2 sse3 ssse3 sse4 sse4.1 sse4.2 mmx)
# -march values at or below x86-64-v2 (no AVX), and those of them that imply POPCNT (x86-64-v2). Anything
# else (haswell, znver2, x86-64-v3, native, ...) counts as enabling every AVX-class feature.
set(_ISA_BASE_ARCHES x86-64 x86-64-v1 x86-64-v2 nehalem westmere core2 penryn k8 k8-sse3 amdfam10 btver1
                     silvermont goldmont goldmont-plus tremont i686 pentium4 nocona generic)
set(_ISA_V2_ARCHES x86-64-v2 nehalem westmere silvermont goldmont goldmont-plus tremont amdfam10 btver1)

# The compiler family of a compile command: msvc (cl), clangcl or gnu (GCC, Clang, MinGW).
function(_isa_family tokens out)
  list(GET tokens 0 exe)
  string(REPLACE "\\" "/" exe "${exe}")
  get_filename_component(name "${exe}" NAME)
  string(TOLOWER "${name}" name)
  string(REGEX REPLACE "\\.exe$" "" name "${name}")
  if(name STREQUAL "cl")
    set(${out} msvc PARENT_SCOPE)
  elseif(name STREQUAL "clang-cl")
    set(${out} clangcl PARENT_SCOPE)
  else()
    set(${out} gnu PARENT_SCOPE)
  endif()
endfunction()

# Evaluates a command's tokens. Sets <out>_<feature> (ON/OFF) for _ISA_FEATURES and popcnt, and
# <out>_march (and <out>_marchtoken, the option as given), <out>_mtune, <out>_msvcarch, <out>_fastmath,
# <out>_contract, <out>_contractoff (an explicit -ffp-contract=off), <out>_stackoff (the last
# stack-protector option turns it off: -fno-stack-protector or /GS-), <out>_sanitize (the -fsanitize=
# options still in effect after the last -fno-sanitize=all), <out>_rtc (MSVC's /RTC options), <out>_coverage
# (the coverage options still in effect, per kind), <out>_abovev1 (every option that enables
# something above x86-64-v1) and <out>_named (the positive -m extension options). Explicit -m options win
# over -march's implied set whatever their order (GCC and Clang); among explicit options the last one wins.
function(_isa_eval_flags tokens family out)
  foreach(f IN LISTS _ISA_FEATURES ITEMS popcnt)
    set(explicit_${f} "")
  endforeach()
  set(march "")
  set(marchToken "")
  set(mtune "")
  set(msvcArch "")
  set(fastmath OFF)
  set(contract OFF)
  set(contractOff OFF)
  set(stackOff OFF)
  set(sanitize "")
  set(rtc "")
  # Coverage instrumentation, one list per kind, because each kind has its own negation (or none).
  set(covDriver "")
  set(covArcs "")
  set(covInstr "")
  set(covGenerate "")
  set(covMapping "")
  set(covSancov "")
  set(aboveV1 "")
  set(named "")
  foreach(t IN LISTS tokens)
    # clang-cl forwards GCC-style options as /clang:<option>.
    if(t MATCHES "^[-/]clang:(.+)$")
      set(t "${CMAKE_MATCH_1}")
    endif()
    if(t MATCHES "^-m(sse3|ssse3|sse4|sse4\\.1|sse4\\.2|sse4a|popcnt|lzcnt|cx16|movbe|avx[a-z0-9.-]*|bmi2?|fma4?|f16c|xop|tbm|adx|aes|pclmul|sha[0-9]*|rdrnd|rdseed|prfchw|abm|xsave[a-z]*|fsgsbase|ptwrite|vpclmulqdq|gfni|vaes)$")
      list(APPEND aboveV1 "${t}")
    elseif(t MATCHES "^-march=")
      if(NOT t MATCHES "^-march=x86-64(-v1)?$")
        list(APPEND aboveV1 "${t}")
      endif()
    elseif(t MATCHES "^[-/]arch:")
      if(NOT t MATCHES "^[-/]arch:(SSE2|IA32)$")
        list(APPEND aboveV1 "${t}")
      endif()
    endif()
    # Positive -m extensions, as given (not -mno-*, and not the -m options that select no instructions).
    if(t MATCHES "^-m([a-z][a-z0-9.]*)$")
      set(ext "${CMAKE_MATCH_1}")
      if(NOT ext MATCHES "^(no-|arch|tune|fpmath|cmodel|abi|stack|red|align|regparm|ms-|long|branch|function|indirect|record|nop|dll|windows|console|thread|crt|64$|32$|x32$|16$)")
        list(APPEND named "${t}")
      endif()
    endif()
    if(t MATCHES "^-march=(.+)$")
      set(march "${CMAKE_MATCH_1}")
      set(marchToken "${t}")
    elseif(t MATCHES "^-mtune=(.+)$")
      set(mtune "${CMAKE_MATCH_1}")
    elseif(t MATCHES "^-fstack-protector(-strong|-all|-explicit)?$" OR t MATCHES "^[-/]GS$")
      set(stackOff OFF)
    elseif(t MATCHES "^-fno-stack-protector$" OR t MATCHES "^[-/]GS-$")
      set(stackOff ON)
    elseif(t MATCHES "^[-/]fsanitize=.")
      list(APPEND sanitize "${t}")
    elseif(t MATCHES "^-fno-sanitize=all$")
      set(sanitize "")
    elseif(t MATCHES "^[-/]RTC[1csu]+$")
      # MSVC's run-time checks (CMake's Debug default /RTC1): there is no option that turns them off again.
      list(APPEND rtc "${t}")
    # Coverage: each negation clears only its own kind, and only what came before it (GCC and Clang).
    # --coverage has none: both drivers keep gcov instrumentation after -fno-profile-arcs (GCC 13, Clang 18).
    # -fno-coverage-mapping drops only the mapping, never the counters of -fprofile-instr-generate.
    # A -fno-sanitize-coverage= may remove some of the kinds a -fsanitize-coverage= enabled, so it clears
    # nothing here.
    elseif(t STREQUAL "--coverage")
      list(APPEND covDriver "${t}")
    elseif(t STREQUAL "-fprofile-arcs")
      list(APPEND covArcs "${t}")
    elseif(t STREQUAL "-fno-profile-arcs")
      set(covArcs "")
    elseif(t MATCHES "^-fprofile-instr-generate(=.*)?$")
      list(APPEND covInstr "${t}")
    elseif(t STREQUAL "-fno-profile-instr-generate")
      set(covInstr "")
    elseif(t MATCHES "^-f(cs-)?profile-generate(=.*)?$")
      list(APPEND covGenerate "${t}")
    elseif(t STREQUAL "-fno-profile-generate") # Clang: it ends -fcs-profile-generate too
      set(covGenerate "")
    elseif(t STREQUAL "-fcoverage-mapping")
      list(APPEND covMapping "${t}")
    elseif(t STREQUAL "-fno-coverage-mapping")
      set(covMapping "")
    elseif(t MATCHES "^[-/]fsanitize-coverage=.")
      list(APPEND covSancov "${t}")
    elseif(t MATCHES "^[-/]arch:(.+)$")
      set(msvcArch "${CMAKE_MATCH_1}")
    elseif(t MATCHES "^-m(no-)?(avx512[a-z0-9]*|avx10[.0-9a-z-]*)$")
      if(CMAKE_MATCH_1)
        set(explicit_avx512 OFF)
      else()
        set(explicit_avx512 ON)
      endif()
    elseif(t MATCHES "^-m(no-)?(avx2|avx|fma|bmi2|bmi|f16c|lzcnt|popcnt)$")
      set(f "${CMAKE_MATCH_2}")
      if(CMAKE_MATCH_1)
        set(explicit_${f} OFF)
        if(f STREQUAL "avx")
          # Disabling AVX disables everything built on it.
          foreach(dep avx2 fma f16c avx512)
            set(explicit_${dep} OFF)
          endforeach()
        elseif(f STREQUAL "avx2")
          set(explicit_avx512 OFF)
        endif()
      else()
        set(explicit_${f} ON)
      endif()
    elseif(t MATCHES "^-ffast-math$" OR t MATCHES "^-Ofast$" OR t MATCHES "^[-/]fp:fast$" OR t MATCHES "^-funsafe-math-optimizations$")
      set(fastmath ON)
    elseif(t MATCHES "^-ffp-contract=(fast|on)$" OR t MATCHES "^[-/]fp:contract$")
      set(contract ON)
      set(contractOff OFF)
    elseif(t MATCHES "^-ffp-contract=off$")
      set(contract OFF)
      set(contractOff ON)
    endif()
  endforeach()
  # Features implied by -march, or by clang-cl's /arch (which selects a CPU: AVX -> sandybridge,
  # AVX2 -> haswell, AVX512 -> skylake-avx512). cl's /arch implies only AVX and AVX2 here: it emits FMA
  # only under /fp:fast or /fp:contract, which fail on their own.
  foreach(f IN LISTS _ISA_FEATURES ITEMS popcnt)
    set(implied_${f} OFF)
  endforeach()
  if(march AND NOT march IN_LIST _ISA_BASE_ARCHES)
    foreach(f IN LISTS _ISA_FEATURES ITEMS popcnt)
      set(implied_${f} ON)
    endforeach()
  elseif(march IN_LIST _ISA_V2_ARCHES)
    set(implied_popcnt ON)
  endif()
  if(msvcArch MATCHES "^AVX")
    set(implied_avx ON)
    if(msvcArch MATCHES "^AVX(2|512|10)")
      set(implied_avx2 ON)
    endif()
    if(msvcArch MATCHES "^AVX(512|10)")
      set(implied_avx512 ON)
    endif()
    if(family STREQUAL "clangcl")
      set(implied_popcnt ON)
      if(msvcArch MATCHES "^AVX(2|512|10)")
        foreach(f fma bmi bmi2 f16c lzcnt)
          set(implied_${f} ON)
        endforeach()
      endif()
    endif()
  endif()
  foreach(f IN LISTS _ISA_FEATURES ITEMS popcnt)
    if(NOT explicit_${f} STREQUAL "")
      set(${out}_${f} ${explicit_${f}} PARENT_SCOPE)
    else()
      set(${out}_${f} ${implied_${f}} PARENT_SCOPE)
    endif()
  endforeach()
  set(${out}_march "${march}" PARENT_SCOPE)
  set(${out}_marchtoken "${marchToken}" PARENT_SCOPE)
  set(${out}_mtune "${mtune}" PARENT_SCOPE)
  set(${out}_msvcarch "${msvcArch}" PARENT_SCOPE)
  set(${out}_fastmath ${fastmath} PARENT_SCOPE)
  set(${out}_contract ${contract} PARENT_SCOPE)
  set(${out}_contractoff ${contractOff} PARENT_SCOPE)
  set(${out}_stackoff ${stackOff} PARENT_SCOPE)
  set(${out}_sanitize "${sanitize}" PARENT_SCOPE)
  set(${out}_rtc "${rtc}" PARENT_SCOPE)
  set(coverage ${covDriver} ${covArcs} ${covInstr} ${covGenerate} ${covMapping} ${covSancov})
  set(${out}_coverage "${coverage}" PARENT_SCOPE)
  set(${out}_abovev1 "${aboveV1}" PARENT_SCOPE)
  set(${out}_named "${named}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------------------------
# Disassembly (GNU or LLVM objdump, Intel syntax)
# ---------------------------------------------------------------------------------------------
# Instructions above x86-64-v1, in Intel-syntax mnemonics.
set(_ISA_FORBIDDEN_MNEMONICS
  "v[a-z0-9]+" # every VEX/EVEX-encoded instruction starts with v (vmovaps, vpxor, vzeroupper...)
  andn bextr blsi blsmsk blsr bzhi pdep pext rorx sarx shlx shrx mulx
  lzcnt tzcnt popcnt movbe cmpxchg16b crc32
  "pshufb|palignr|phadd[wd]|phaddsw|phsub[wd]|phsubsw|pmaddubsw|pmulhrsw|psign[bwd]|pabs[bwd]"
  "ptest|pblendw|pblendvb|blendps|blendpd|blendvps|blendvpd|round[sp][sd]|pminsb|pminsd|pminuw|pminud|pmaxsb|pmaxsd|pmaxuw|pmaxud"
  "pmulld|pmuldq|pinsr[bdq]|pextr[bdq]|pcmpeqq|pcmpgtq|packusdw|pmov[sz]x[a-z]+|dpp[sd]|insertps|extractps"
  "mpsadbw|phminposuw|pcmp[ei]str[im]|movntdqa"
  "hadd[sp][sd]|hsub[sp][sd]|addsub[sp][sd]|movddup|movshdup|movsldup|lddqu|fisttp"
  # AVX-512 opmask instructions (not v-prefixed), AES-NI, PCLMULQDQ, SHA, ADX, RDRAND/RDSEED,
  # 3DNow!/PRFCHW, XSAVE family (xgetbv itself is allowed: the probe needs it).
  "k(mov|and|andn|or|xor|xnor|not|ortest|test|shiftl|shiftr|unpck|add)[bwdq]+"
  "aes[a-z0-9]*|pclmul[a-z]*|sha1[a-z0-9]*|sha256[a-z0-9]*|adcx|adox|rdrand|rdseed|prefetchw|prefetchwt1"
  "xsave[a-z0-9]*|xrstor[a-z0-9]*|extrq|insertq|movnts[sd]")
string(JOIN "|" _ISA_FORBIDDEN_ALT ${_ISA_FORBIDDEN_MNEMONICS})
# Instruction prefixes objdump prints as separate words ("lock cmpxchg16b ...", "rep stos ...").
set(_ISA_PREFIX_WORDS lock rep repz repnz repe repne data16 data32 addr32 addr16 cs ds es ss fs gs notrack bnd
                      xacquire xrelease rex rex.w)

# Disassembles `file` and sets <out>_hits to "<symbol>|<mnemonic>|<instruction>" for every instruction
# above x86-64-v1 (symbol: the enclosing `<symbol>:` label; mnemonic: prefixes skipped), <out>_count to the
# number of instructions and <out>_error to a failure message or "".
function(_isa_disassemble file out)
  set(${out}_hits "" PARENT_SCOPE)
  set(${out}_count 0 PARENT_SCOPE)
  execute_process(COMMAND "${OBJDUMP}" -d --no-show-raw-insn -M intel "${file}"
                  OUTPUT_VARIABLE dis ERROR_VARIABLE disErr RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    set(${out}_error "${OBJDUMP} failed on '${file}': ${disErr}" PARENT_SCOPE)
    return()
  endif()
  set(${out}_error "" PARENT_SCOPE)
  string(REPLACE ";" "," dis "${dis}")
  string(REPLACE "[" "(" dis "${dis}")
  string(REPLACE "]" ")" dis "${dis}")
  string(REGEX MATCHALL "[^\n]+" lines "${dis}")
  set(count 0)
  set(hits "")
  set(sym "")
  foreach(line IN LISTS lines)
    # GNU objdump: "   1f:\tmov    eax,0x1"; llvm-objdump: "      1f:      \tmov\teax, 0x1".
    if(line MATCHES "^ *[0-9a-f]+:[ \t]+([a-z0-9.]+)([ \t]+(.*))?$")
      math(EXPR count "${count} + 1")
      set(mn "${CMAKE_MATCH_1}")
      set(ops "${CMAKE_MATCH_3}")
      # Skip prefixes so "lock cmpxchg16b" is judged by its instruction.
      while(mn IN_LIST _ISA_PREFIX_WORDS AND ops MATCHES "^([a-z0-9.]+)([ \t]+(.*))?$")
        set(mn "${CMAKE_MATCH_1}")
        set(ops "${CMAKE_MATCH_3}")
      endwhile()
      if(mn MATCHES "^(${_ISA_FORBIDDEN_ALT})$" OR ops MATCHES "(^|[^a-z0-9_])([yz]mm[0-9]|k[1-7]([^a-z0-9_]|$))")
        string(STRIP "${line}" shown)
        string(REPLACE "|" "/" shown "${shown}")
        list(APPEND hits "${sym}|${mn}|${shown}")
      endif()
    elseif(line MATCHES "^[0-9a-f]+ <(.+)>:$")
      set(sym "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  set(${out}_hits "${hits}" PARENT_SCOPE)
  set(${out}_count ${count} PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------------------------
# 2. Gate objects (GNU binutils)
# ---------------------------------------------------------------------------------------------
function(_isa_check_object obj label)
  set(errs "")
  if(NOT EXISTS "${obj}")
    list(APPEND errs "${label}: object '${obj}' does not exist (build the target first)")
  else()
    if(OBJDUMP)
      _isa_disassemble("${obj}" d)
      if(d_error)
        list(APPEND errs "${label}: ${d_error}")
      elseif(d_count EQUAL 0)
        list(APPEND errs "${label}: no instructions disassembled from '${obj}' (unexpected objdump output)")
      endif()
      foreach(hit IN LISTS d_hits)
        string(REGEX MATCH "^[^|]*\\|[^|]*\\|(.*)$" fields "${hit}")
        set(shown "${CMAKE_MATCH_1}")
        list(APPEND errs "${label}: instruction not allowed at the x86-64-v1 baseline (VEX/EVEX, BMI, LZCNT, POPCNT, SSE3+): ${shown}")
      endforeach()
    endif()
    if(NM)
      execute_process(COMMAND "${NM}" "${obj}" OUTPUT_VARIABLE syms ERROR_VARIABLE nmErr RESULT_VARIABLE rc)
      if(NOT rc EQUAL 0)
        list(APPEND errs "${label}: ${NM} failed on '${obj}': ${nmErr}")
      else()
        string(REPLACE ";" "," syms "${syms}")
        string(REGEX MATCHALL "[^\n]+" lines "${syms}")
        foreach(line IN LISTS lines)
          if(line MATCHES "^ *U ([^ ]+)$")
            set(name "${CMAKE_MATCH_1}")
            if(name IN_LIST HELIOS_ISA_GATE_ALLOWED_IMPORTS)
            elseif(name MATCHES "^_*(stack_chk_|security_cookie|security_check_cookie|GSHandlerCheck|asan_|ubsan_|sanitizer_|tsan_|msan_|hwasan_|llvm_gcov|gcov_|llvm_profile)")
              list(APPEND errs "${label}: references '${name}': the gate objects run before the CRT and the sanitizer runtimes exist, so they build without a stack protector or instrumentation (02 §1.1)")
            else()
              list(APPEND errs "${label}: references '${name}', which is not on HELIOS_ISA_GATE_ALLOWED_IMPORTS")
            endif()
          elseif(line MATCHES "^[0-9a-fA-F]+ ([A-Za-z]) ([^ ]+)$")
            set(type "${CMAKE_MATCH_1}")
            set(name "${CMAKE_MATCH_2}")
            if(type MATCHES "^[WwVvui]$")
              list(APPEND errs "${label}: weak/COMDAT-unique/IFUNC symbol '${name}' (type ${type}): gate code must be static")
            elseif(type MATCHES "^[TDBRC]$" AND NOT name IN_LIST HELIOS_ISA_GATE_EXPORTS)
              list(APPEND errs "${label}: exports '${name}' (type ${type}): only HELIOS_ISA_GATE_EXPORTS may be external")
            endif()
          endif()
        endforeach()
      endif()
    endif()
  endif()
  set(_isa_object_errors "${errs}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------------------------
# 3. Gated ELF executables: .preinit_array holds exactly one entry, the gate's hook; no IFUNC
#    relocation (an IFUNC resolver would run at relocation time, before the gate).
# ---------------------------------------------------------------------------------------------
function(_isa_check_image img)
  set(errs "")
  if(NOT EXISTS "${img}")
    list(APPEND errs "${img}: gated executable not built")
    set(_isa_image_errors "${errs}" PARENT_SCOPE)
    return()
  endif()
  execute_process(COMMAND "${READELF}" -W -S "${img}" OUTPUT_VARIABLE sections RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    list(APPEND errs "${img}: readelf -S failed")
    set(_isa_image_errors "${errs}" PARENT_SCOPE)
    return()
  endif()
  if(sections MATCHES "\\.preinit_array +PREINIT_ARRAY +([0-9a-f]+) +[0-9a-f]+ +([0-9a-f]+)")
    set(addr "${CMAKE_MATCH_1}")
    set(size "${CMAKE_MATCH_2}")
    if(NOT size MATCHES "^0*8$")
      list(APPEND errs "${img}: .preinit_array has size 0x${size}, but it must hold exactly the CPU gate (8 bytes)")
    elseif(NM)
      # The entry's target: the R_X86_64_RELATIVE addend at the slot (PIE), else the slot's content.
      string(REGEX REPLACE "^0+" "" addrKey "${addr}")
      execute_process(COMMAND "${READELF}" -W -r "${img}" OUTPUT_VARIABLE relocs)
      set(target "")
      string(REGEX MATCHALL "[^\n]+" relocLines "${relocs}")
      foreach(line IN LISTS relocLines)
        if(line MATCHES "^0*${addrKey} +[0-9a-f]+ +R_X86_64_RELATIVE +([0-9a-f]+)")
          set(target "${CMAKE_MATCH_1}")
        endif()
      endforeach()
      if(target STREQUAL "")
        execute_process(COMMAND "${READELF}" -W -x .preinit_array "${img}" OUTPUT_VARIABLE dump)
        if(dump MATCHES "0x[0-9a-f]+ ([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f]) ([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])")
          set(target "${CMAKE_MATCH_8}${CMAKE_MATCH_7}${CMAKE_MATCH_6}${CMAKE_MATCH_5}${CMAKE_MATCH_4}${CMAKE_MATCH_3}${CMAKE_MATCH_2}${CMAKE_MATCH_1}")
        endif()
      endif()
      string(REGEX REPLACE "^0+" "" target "${target}")
      execute_process(COMMAND "${NM}" "${img}" OUTPUT_VARIABLE syms)
      set(gateAddr "")
      set(sym "${HELIOS_PRE_MAIN_GATE_PREINIT_SYMBOL}")
      if(syms MATCHES "(^|\n)([0-9a-f]+) [tT] ${sym}(\n|$)")
        string(REGEX REPLACE "^0+" "" gateAddr "${CMAKE_MATCH_2}")
      endif()
      if(gateAddr STREQUAL "")
        list(APPEND errs "${img}: no ${sym} symbol (stripped image, or the CPU gate hook is not linked)")
      elseif(NOT target STREQUAL gateAddr)
        list(APPEND errs "${img}: the .preinit_array entry (0x${target}) is not the CPU gate (${sym} at 0x${gateAddr})")
      endif()
    endif()
  else()
    list(APPEND errs "${img}: no .preinit_array: the CPU gate is not linked (helios_cpu_gate)")
  endif()
  execute_process(COMMAND "${READELF}" -W -r "${img}" OUTPUT_VARIABLE relocs RESULT_VARIABLE rc)
  if(relocs MATCHES "R_X86_64_IRELATIVE")
    list(APPEND errs "${img}: R_X86_64_IRELATIVE relocation (an IFUNC resolver would run before the CPU gate)")
  endif()
  set(_isa_image_errors "${errs}" PARENT_SCOPE)
endfunction()

# 3, shared libraries a gated executable loads (ELF): no IFUNC relocation. ld.so relocates every object of
# the start-up set before it runs the executable's .preinit_array, so an IFUNC resolver in a group library
# (modular dev builds) would run before the CPU gate, as one in the executable would.
function(_isa_check_shared_image img)
  set(errs "")
  if(NOT EXISTS "${img}")
    set(_isa_shared_errors "${img}: shared library not built" PARENT_SCOPE)
    return()
  endif()
  execute_process(COMMAND "${READELF}" -W -r "${img}" OUTPUT_VARIABLE relocs RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    list(APPEND errs "${img}: readelf -r failed")
  elseif(relocs MATCHES "R_X86_64_IRELATIVE")
    list(APPEND errs "${img}: R_X86_64_IRELATIVE relocation in a shared library that gated executables load (an IFUNC resolver would run before the CPU gate)")
  endif()
  set(_isa_shared_errors "${errs}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------------------------
# 4. Base images: every instruction above x86-64-v1 belongs to a listed self-dispatching symbol.
#    Catches intrinsics, target attributes and COMDAT picks at link time (02 §1.1).
# ---------------------------------------------------------------------------------------------
function(_isa_check_base_image img)
  set(errs "")
  set(listed "")
  if(NOT EXISTS "${img}")
    set(_isa_base_errors "${img}: base image not built" PARENT_SCOPE)
    set(_isa_base_listed "" PARENT_SCOPE)
    return()
  endif()
  _isa_disassemble("${img}" d)
  if(d_error)
    list(APPEND errs "${img}: ${d_error}")
  elseif(d_count EQUAL 0)
    list(APPEND errs "${img}: no instructions disassembled (unexpected objdump output)")
  endif()
  # One entry per symbol: its name, its number of such instructions and the first one (parallel lists).
  # TZCNT's encoding is the exception: GCC and Clang emit it (`rep bsf`) for count-trailing-zeros at
  # x86-64-v1 with -mtune=generic, where the compiler knows the operand is non-zero, and a CPU without
  # BMI1 executes it as BSF with the same result. It is counted, not reported. LZCNT has no such idiom.
  # The bytes cannot tell that idiom from a real TZCNT, so this is a known false negative: code built for
  # BMI1 (a target("bmi") attribute or pragma, or inline assembly) that relies on TZCNT's result for a zero
  # operand passes here and computes a wrong value on a CPU without BMI1. MODE=base_sources rejects those
  # forms in the Helios base modules' sources; third-party base libraries are not scanned.
  set(symbols "")
  set(counts "")
  set(firsts "")
  set(repBsf 0)
  foreach(hit IN LISTS d_hits)
    # (string(REGEX REPLACE) would apply a ^ anchor at every match, so the fields are captured.)
    string(REGEX MATCH "^([^|]*)\\|([^|]*)\\|(.*)$" fields "${hit}")
    set(sym "${CMAKE_MATCH_1}")
    set(mn "${CMAKE_MATCH_2}")
    set(shown "${CMAKE_MATCH_3}")
    if(mn STREQUAL "tzcnt")
      math(EXPR repBsf "${repBsf} + 1")
      continue()
    endif()
    if(sym STREQUAL "")
      set(sym "<no symbol>")
    endif()
    list(FIND symbols "${sym}" at)
    if(at EQUAL -1)
      list(APPEND symbols "${sym}")
      list(APPEND counts 1)
      list(APPEND firsts "${shown}")
    else()
      list(GET counts ${at} c)
      math(EXPR c "${c} + 1")
      list(REMOVE_AT counts ${at})
      list(INSERT counts ${at} ${c})
    endif()
  endforeach()
  set(unlisted "")
  set(i 0)
  foreach(sym IN LISTS symbols)
    # Compiler clones (foo.constprop.0, foo.isra.0, foo.part.0, foo.cold) and versioned names count
    # as the function they come from.
    string(REGEX REPLACE "[.@].*$" "" base "${sym}")
    if(base IN_LIST HELIOS_ISA_SELF_DISPATCH_SYMBOLS)
      list(APPEND listed "${sym}")
    else()
      list(GET counts ${i} c)
      list(GET firsts ${i} shown)
      list(APPEND unlisted "${sym} (${c} instruction(s), first: ${shown})")
    endif()
    math(EXPR i "${i} + 1")
  endforeach()
  if(unlisted)
    list(LENGTH unlisted n)
    string(REPLACE ";" "; " text "${unlisted}")
    list(APPEND errs "${img}: base image: ${n} symbol(s) with instructions above x86-64-v1 that are not on HELIOS_ISA_SELF_DISPATCH_SYMBOLS: ${text}")
  endif()
  set(_isa_base_errors "${errs}" PARENT_SCOPE)
  set(_isa_base_listed "${listed}" PARENT_SCOPE)
  set(_isa_base_repbsf ${repBsf} PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------------------------
# Check 4's complement: the Helios base modules' sources (02 §1.1). Check 4 accepts TZCNT's encoding as
# BSF, so it cannot see code that was built for BMI1 and relies on TZCNT for a zero operand. Such code
# needs a target attribute or pragma that enables BMI (GCC rejects _tzcnt_u32 without one), a TZCNT
# intrinsic or inline assembly; this scan rejects all three in the given directories. It is a text scan:
# a macro that expands to a target attribute (zstd's BMI2_TARGET_ATTRIBUTE, say) is not seen, which is why
# it covers Helios's own modules only. Comments (// and lines that start a /* or * block) are skipped.
# ---------------------------------------------------------------------------------------------
function(_isa_scan_base_sources dirs)
  set(errs "")
  set(scanned 0)
  foreach(d IN LISTS dirs)
    if(NOT IS_DIRECTORY "${d}")
      list(APPEND errs "${d}: not a directory")
      continue()
    endif()
    file(GLOB_RECURSE files LIST_DIRECTORIES false
         "${d}/*.c" "${d}/*.cc" "${d}/*.cpp" "${d}/*.cxx" "${d}/*.h" "${d}/*.hh" "${d}/*.hpp" "${d}/*.hxx"
         "${d}/*.inl" "${d}/*.ipp")
    list(SORT files)
    foreach(f IN LISTS files)
      math(EXPR scanned "${scanned} + 1")
      file(READ "${f}" text)
      # Protect what would change CMake's list splitting (a line-continuation backslash would escape
      # the separator that replaces its newline); the placeholders are undone in the report.
      string(REPLACE "\\" "<BS>" text "${text}")
      string(REPLACE ";" "<SEMI>" text "${text}")
      string(REPLACE "[" "<LB>" text "${text}")
      string(REPLACE "]" "<RB>" text "${text}")
      string(REPLACE "\r" "" text "${text}")
      string(REPLACE "\n" ";" lines "${text}")
      set(n 0)
      foreach(line IN LISTS lines)
        math(EXPR n "${n} + 1")
        string(REGEX REPLACE "//.*$" "" code "${line}")
        if(code MATCHES "^[ \t]*(/\\*|\\*)")
          continue()
        endif()
        set(what "")
        if(code MATCHES "(^|[^A-Za-z0-9_])(__)?target(_clones)?(__)?[ \t]*\\([ \t]*\"[^\"]*(bmi|arch=)")
          set(what "a target attribute or pragma that enables BMI")
        elseif(code MATCHES "(^|[^A-Za-z0-9_])(_+tzcnt|_mm_tzcnt|__builtin_ia32_tzcnt)")
          set(what "a TZCNT intrinsic")
        elseif(code MATCHES "\"[^\"]*(tzcnt|rep[ \t]*bsf)")
          set(what "TZCNT in inline assembly")
        endif()
        if(what)
          string(STRIP "${code}" shown)
          list(APPEND errs "${f}:${n}: ${what} (${shown}): check 4 reads TZCNT as BSF, so a base image cannot rely on TZCNT for a zero operand")
        endif()
      endforeach()
    endforeach()
  endforeach()
  set(_isa_sources_errors "${errs}" PARENT_SCOPE)
  set(_isa_sources_scanned ${scanned} PARENT_SCOPE)
endfunction()

if(MODE STREQUAL "base_sources")
  string(REPLACE "|" ";" dirs "${SOURCE_DIRS}")
  if(NOT dirs)
    message(FATAL_ERROR "isa_audit: MODE=base_sources needs SOURCE_DIRS (directories separated by |)")
  endif()
  _isa_scan_base_sources("${dirs}")
  if(_isa_sources_errors)
    list(LENGTH _isa_sources_errors n)
    string(REPLACE ";" "\n  " text "${_isa_sources_errors}")
    string(REPLACE "<SEMI>" ";" text "${text}")
    string(REPLACE "<LB>" "[" text "${text}")
    string(REPLACE "<RB>" "]" text "${text}")
    string(REPLACE "<BS>" "\\" text "${text}")
    message(FATAL_ERROR "ISA audit failed (${n} finding(s) in base-module sources):\n  ${text}\n")
  endif()
  message(STATUS "ISA audit: ${_isa_sources_scanned} base-module source files hold no BMI target, TZCNT intrinsic "
                 "or TZCNT assembly")
  return()
endif()

if(MODE STREQUAL "image")
  if(NOT READELF)
    message(FATAL_ERROR "isa_audit: MODE=image needs READELF")
  endif()
  _isa_check_image("${IMAGE}")
  if(_isa_image_errors)
    string(REPLACE ";" "\n  " text "${_isa_image_errors}")
    message(FATAL_ERROR "ISA audit failed:\n  ${text}\n")
  endif()
  message(STATUS "ISA audit: '${IMAGE}' starts with the CPU gate")
  return()
endif()

if(MODE STREQUAL "shared_image")
  if(NOT READELF)
    message(FATAL_ERROR "isa_audit: MODE=shared_image needs READELF")
  endif()
  _isa_check_shared_image("${IMAGE}")
  if(_isa_shared_errors)
    string(REPLACE ";" "\n  " text "${_isa_shared_errors}")
    message(FATAL_ERROR "ISA audit failed:\n  ${text}\n")
  endif()
  message(STATUS "ISA audit: '${IMAGE}' has no IFUNC relocation")
  return()
endif()

if(MODE STREQUAL "object")
  _isa_check_object("${OBJECT}" "object")
  if(_isa_object_errors)
    string(REPLACE ";" "\n  " text "${_isa_object_errors}")
    message(FATAL_ERROR "ISA audit failed:\n  ${text}\n")
  endif()
  message(STATUS "ISA audit: '${OBJECT}' is clean")
  return()
endif()

if(MODE STREQUAL "base_image")
  if(NOT OBJDUMP)
    message(FATAL_ERROR "isa_audit: MODE=base_image needs OBJDUMP")
  endif()
  _isa_check_base_image("${IMAGE}")
  if(_isa_base_errors)
    string(REPLACE ";" "\n  " text "${_isa_base_errors}")
    message(FATAL_ERROR "ISA audit failed:\n  ${text}\n")
  endif()
  list(LENGTH _isa_base_listed n)
  message(STATUS "ISA audit: base image '${IMAGE}' is x86-64-v1 outside ${n} listed self-dispatching symbol(s) "
                 "(${_isa_base_repbsf} TZCNT-encoded BSF accepted)")
  return()
endif()

# ---------------------------------------------------------------------------------------------
# 1. Flags
# ---------------------------------------------------------------------------------------------
if(NOT COMPILE_COMMANDS OR NOT EXISTS "${COMPILE_COMMANDS}")
  message(FATAL_ERROR "isa_audit: '${COMPILE_COMMANDS}' not found. The audit needs a Ninja or Makefile build "
                      "(CMAKE_EXPORT_COMPILE_COMMANDS); Visual Studio generator builds are audited in CI with the "
                      "windows-msvc-release preset.")
endif()
if(NOT LEVELS OR NOT EXISTS "${LEVELS}")
  message(FATAL_ERROR "isa_audit: LEVELS (helios_generated/isa_levels.txt, written at configure time by "
                      "cmake/HeliosIsa.cmake) not given or missing: '${LEVELS}'")
endif()
file(STRINGS "${LEVELS}" levelLines)
foreach(line IN LISTS levelLines)
  if(line MATCHES "^([^ ]+) (avx2|base|gate)$")
    set("_isaLevel_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
  endif()
endforeach()

file(READ "${COMPILE_COMMANDS}" json)
# One key per line in CMake's output; protect list separators before splitting into lines.
# (Unbalanced square brackets would also stop CMake's list splitting.)
string(REPLACE ";" "<SEMI>" json "${json}")
string(REPLACE "[" "<LB>" json "${json}")
string(REPLACE "]" "<RB>" json "${json}")
string(REGEX MATCHALL "[^\n]+" jsonLines "${json}")

# The CPU gate's two sources (02 §1.1): built only in gate-level object libraries.
set(_ISA_GATE_SOURCES "/engine/core/src/cpugate/[^/]+\\.c$" "/engine/core/src/platform/(posix|win32)/cpu_gate_hook\\.c$")

set(entries 0)
set(skipped 0)
set(levelCounts_avx2 0)
set(levelCounts_base 0)
set(gateUnits "")
set(dir "")
set(cmd "")
set(file "")
set(output "")
macro(_isa_flush_entry)
  # C and C++ units only: a resource script (rc, windres) takes no ISA flags.
  if(NOT file STREQUAL "" AND NOT file MATCHES "\\.(c|cc|cpp|cxx|c\\+\\+|C)$")
    math(EXPR skipped "${skipped} + 1")
  elseif(NOT file STREQUAL "")
    math(EXPR entries "${entries} + 1")
    string(REPLACE "\\\\" "/" fileN "${file}")
    string(REPLACE "\\\\" "/" outN "${output}")
    string(REPLACE "\\\"" "\"" cmdN "${cmd}")
    string(REPLACE "<LB>" "[" cmdN "${cmdN}")
    string(REPLACE "<RB>" "]" cmdN "${cmdN}")
    set(target "")
    if(outN MATCHES "CMakeFiles/([^/]+)\\.dir/")
      set(target "${CMAKE_MATCH_1}")
    endif()
    separate_arguments(tokens NATIVE_COMMAND "${cmdN}")
    _isa_family("${tokens}" family)
    _isa_eval_flags("${tokens}" ${family} f)
    set(enabled "")
    foreach(feat IN LISTS _ISA_FEATURES)
      if(f_${feat})
        list(APPEND enabled ${feat})
      endif()
    endforeach()
    set(level "${_isaLevel_${target}}")
    set(unit "${fileN} [${target}]")
    set(isGateSource OFF)
    foreach(p IN LISTS _ISA_GATE_SOURCES)
      if(fileN MATCHES "${p}")
        set(isGateSource ON)
      endif()
    endforeach()
    if(f_march STREQUAL "native")
      _violation("${unit}: -march=native (non-reproducible binaries, use the ISA levels of cmake/HeliosIsa.cmake)")
    endif()
    if(f_fastmath)
      _violation("${unit}: fast-math flags break cross-platform determinism")
    endif()
    if(f_contract)
      _violation("${unit}: FP contraction enabled (-ffp-contract=fast|on or /fp:contract), and Helios never fuses a*b+c")
    endif()
    if(isGateSource AND NOT level STREQUAL "gate")
      _violation("${unit}: CPU-gate unit compiled in a target of level '${level}': the gate's units are built only in gate-level object libraries (helios_cpu_gate_target)")
    endif()
    if(level STREQUAL "")
      _violation("${unit}: target '${target}' has no ISA level (not in ${LEVELS}), but helios_isa_finalize() gives every target one at configure time")
    elseif(level STREQUAL "avx2")
      math(EXPR levelCounts_avx2 "${levelCounts_avx2} + 1")
      set(missing "")
      if(family STREQUAL "msvc")
        if(NOT f_msvcarch STREQUAL "AVX2")
          _violation("${unit}: avx2 unit built below its image's level (missing: /arch:AVX2) (02 §1.1)")
        endif()
      else()
        foreach(feat IN LISTS _ISA_AVX2_REQUIRED)
          if(NOT f_${feat})
            list(APPEND missing ${feat})
          endif()
        endforeach()
        if(NOT f_contractoff)
          list(APPEND missing "-ffp-contract=off")
        endif()
      endif()
      if(missing)
        string(REPLACE ";" ", " text "${missing}")
        _violation("${unit}: avx2 unit built below its image's level (missing: ${text}) (02 §1.1)")
      endif()
      if(f_avx512)
        _violation("${unit}: AVX-512 enabled, which the avx2 level forbids")
      endif()
      if(f_fma)
        _violation("${unit}: FMA enabled (avx2 units must pass -mno-fma for determinism, 02 §7.1)")
      endif()
      set(extra "")
      foreach(opt IN LISTS f_named)
        string(REGEX REPLACE "^-m" "" ext "${opt}")
        if(NOT ext IN_LIST _ISA_AVX2_NAMED AND NOT ext MATCHES "^(fma|avx512.*|avx10.*)$")
          list(APPEND extra "${opt}")
        endif()
      endforeach()
      # The set selects no CPU: an -march beyond x86-64 adds what that CPU has (native is reported above).
      if(f_march AND NOT f_march MATCHES "^(x86-64|x86-64-v1|native)$")
        list(APPEND extra "${f_marchtoken}")
      endif()
      if(extra)
        string(REPLACE ";" " " text "${extra}")
        _violation("${unit}: flags outside the avx2 level set (${text})")
      endif()
    elseif(level MATCHES "^(base|gate)$")
      if(level STREQUAL "gate")
        set(what "CPU gate")
        list(APPEND gateUnits "${fileN}")
      else()
        set(what "base image")
        math(EXPR levelCounts_base "${levelCounts_base} + 1")
      endif()
      if(enabled)
        string(REPLACE ";" ", " en "${enabled}")
        _violation("${unit}: baseline unit (${what}) compiled with AVX-class flags (${en})")
      elseif(f_abovev1)
        string(REPLACE ";" " " en "${f_abovev1}")
        _violation("${unit}: baseline unit (${what}) compiled with flags above x86-64-v1 (${en})")
      endif()
      # The level set itself must be there, not only nothing above it: GCC, Clang and MinGW otherwise
      # build for the toolchain's default -march, which is x86-64-v2 on RHEL 9, x86-64-v3 on RHEL 10 and
      # anything a --with-arch build chose. (x64 MSVC and clang-cl have no base flags: /arch:SSE2 is the
      # default.) The gate adds its object rules: no stack protector or /GS cookie, no instrumentation.
      set(missing "")
      if(family STREQUAL "gnu")
        if(f_march STREQUAL "")
          list(APPEND missing "-march=x86-64")
        endif()
        if(NOT f_mtune STREQUAL "generic")
          list(APPEND missing "-mtune=generic")
        endif()
      endif()
      if(level STREQUAL "gate" AND NOT f_stackoff)
        if(family STREQUAL "gnu")
          list(APPEND missing "-fno-stack-protector")
        else()
          list(APPEND missing "/GS-")
        endif()
      endif()
      if(missing)
        string(REPLACE ";" ", " text "${missing}")
        _violation("${unit}: baseline unit (${what}) built without its level set (missing: ${text}) (02 §1.1)")
      endif()
      if(level STREQUAL "gate" AND f_sanitize)
        string(REPLACE ";" " " text "${f_sanitize}")
        _violation("${unit}: CPU-gate unit built with sanitizer instrumentation (${text}), whose runtime does not exist yet when the gate runs (02 §1.1)")
      endif()
      if(level STREQUAL "gate" AND f_rtc)
        string(REPLACE ";" " " text "${f_rtc}")
        _violation("${unit}: CPU-gate unit built with MSVC run-time checks (${text}), which call the CRT before it exists (02 §1.1)")
      endif()
      if(level STREQUAL "gate" AND f_coverage)
        string(REPLACE ";" " " text "${f_coverage}")
        _violation("${unit}: CPU-gate unit built with coverage instrumentation (${text}), whose runtime does not exist yet when the gate runs (02 §1.1)")
      endif()
      if(level STREQUAL "gate" AND NOT SKIP_OBJECTS)
        set(obj "${outN}")
        if(NOT IS_ABSOLUTE "${obj}")
          string(REPLACE "\\\\" "/" dirN "${dir}")
          set(obj "${dirN}/${obj}")
        endif()
        _isa_check_object("${obj}" "${unit}")
        foreach(e IN LISTS _isa_object_errors)
          _violation("${e}")
        endforeach()
      endif()
    endif()
  endif()
  set(dir "")
  set(cmd "")
  set(file "")
  set(output "")
endmacro()

foreach(line IN LISTS jsonLines)
  if(line MATCHES "^[ \t]*\\{")
    _isa_flush_entry()
  elseif(line MATCHES "^[ \t]*\"directory\"[ \t]*:[ \t]*\"(.*)\",?[ \t]*$")
    set(dir "${CMAKE_MATCH_1}")
  elseif(line MATCHES "^[ \t]*\"command\"[ \t]*:[ \t]*\"(.*)\",?[ \t]*$")
    set(cmd "${CMAKE_MATCH_1}")
  elseif(line MATCHES "^[ \t]*\"file\"[ \t]*:[ \t]*\"(.*)\",?[ \t]*$")
    set(file "${CMAKE_MATCH_1}")
  elseif(line MATCHES "^[ \t]*\"output\"[ \t]*:[ \t]*\"(.*)\",?[ \t]*$")
    set(output "${CMAKE_MATCH_1}")
  endif()
endforeach()
_isa_flush_entry()

if(entries EQUAL 0)
  _violation("${COMPILE_COMMANDS}: no compile commands found")
endif()
list(LENGTH gateUnits gateCount)
if(REQUIRE_GATE AND gateCount EQUAL 0)
  _violation("no CPU-gate unit (a unit of a gate-level target) found in ${COMPILE_COMMANDS}")
endif()

# ---------------------------------------------------------------------------------------------
# 3. Gated executables (ELF)
# ---------------------------------------------------------------------------------------------
set(imageCount 0)
if(IMAGES_FILE AND EXISTS "${IMAGES_FILE}" AND READELF AND NOT SKIP_OBJECTS)
  file(STRINGS "${IMAGES_FILE}" images)
  foreach(img IN LISTS images)
    if(img STREQUAL "" OR img MATCHES "\\.exe$")
      continue()
    endif()
    math(EXPR imageCount "${imageCount} + 1")
    _isa_check_image("${img}")
    foreach(e IN LISTS _isa_image_errors)
      _violation("${e}")
    endforeach()
  endforeach()
endif()

set(sharedImageCount 0)
if(SHARED_IMAGES_FILE AND EXISTS "${SHARED_IMAGES_FILE}" AND READELF AND NOT SKIP_OBJECTS)
  file(STRINGS "${SHARED_IMAGES_FILE}" sharedImages)
  foreach(img IN LISTS sharedImages)
    if(img STREQUAL "" OR img MATCHES "\\.dll$")
      continue()
    endif()
    math(EXPR sharedImageCount "${sharedImageCount} + 1")
    _isa_check_shared_image("${img}")
    foreach(e IN LISTS _isa_shared_errors)
      _violation("${e}")
    endforeach()
  endforeach()
endif()

# ---------------------------------------------------------------------------------------------
# 4. Base images (GNU binutils; COFF images and PDBs are WP-0.2r part 2's)
# ---------------------------------------------------------------------------------------------
set(baseImageCount 0)
if(BASE_IMAGES_FILE AND EXISTS "${BASE_IMAGES_FILE}" AND OBJDUMP AND NOT SKIP_OBJECTS)
  file(STRINGS "${BASE_IMAGES_FILE}" baseImages)
  foreach(img IN LISTS baseImages)
    if(img STREQUAL "" OR img MATCHES "\\.exe$")
      continue()
    endif()
    math(EXPR baseImageCount "${baseImageCount} + 1")
    _isa_check_base_image("${img}")
    foreach(e IN LISTS _isa_base_errors)
      _violation("${e}")
    endforeach()
  endforeach()
endif()

if(violations)
  list(LENGTH violations n)
  string(REPLACE ";" "\n  " text "${violations}")
  string(REPLACE "<SEMI>" ";" text "${text}")
  string(REPLACE "<LB>" "[" text "${text}")
  string(REPLACE "<RB>" "]" text "${text}")
  message(FATAL_ERROR "ISA audit failed (${n} violation(s); rules in docs/plan/02-engine-runtime.md §1.1, lists in cmake/isa_allowlist.cmake):\n  ${text}\n")
endif()
message(STATUS "ISA audit passed: ${entries} units (${levelCounts_avx2} avx2, ${levelCounts_base} base, ${gateCount} CPU-gate), "
               "${imageCount} gated executables, ${sharedImageCount} shared libraries they load, "
               "${baseImageCount} base images")
