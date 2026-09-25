# Embeds a text file into a C++ source as a NUL-terminated byte array.
#   cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DSYMBOL=<name> -P embed.cmake
# Bytes (not a string literal) because MSVC limits string literals to 64 KiB.
file(READ "${INPUT}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" bytes "${bytes}")
get_filename_component(name "${INPUT}" NAME)
file(WRITE "${OUTPUT}.tmp"
  "// Generated from ${name} by tools/schemac/embed.cmake. DO NOT EDIT.\n"
  "namespace helios::schemac {\n"
  "extern const unsigned char ${SYMBOL}[] = {\n${bytes}0x00};\n"
  "} // namespace helios::schemac\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
