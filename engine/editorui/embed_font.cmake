# Embeds a font file into a C++ source as a byte array (helios::edui::detail).
#   cmake -DINPUT=<font.ttf> -DOUTPUT=<file.cpp> -DSYMBOL=<name> -P embed_font.cmake
# Bytes, not a string literal, because MSVC limits string literals to 64 KiB.
file(READ "${INPUT}" hex HEX)
file(SIZE "${INPUT}" size)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" bytes "${bytes}")
get_filename_component(name "${INPUT}" NAME)
file(WRITE "${OUTPUT}.tmp"
  "// Generated from ${name} by engine/editorui/embed_font.cmake. DO NOT EDIT.\n"
  "#include \"embedded_font.h\"\n\n"
  "namespace helios::edui::detail {\n"
  "extern const unsigned char ${SYMBOL}[] = {\n${bytes}};\n"
  "extern const usize ${SYMBOL}Size = ${size};\n"
  "} // namespace helios::edui::detail\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
