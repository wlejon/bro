# embed_js.cmake — Embed any text file into a C++ header as a null-terminated
# byte array. Generic despite the name: driven by INPUT / OUTPUT / VAR_NAME and
# used for JS polyfills (src/js, src/engine) and GLSL shaders (src/scene). The
# brokit sibling also resolves this file via ${CMAKE_SOURCE_DIR} when built in
# tree, so keep the filename stable.
#
# Usage (from add_custom_command):
#   cmake -DINPUT=mesh.vert -DOUTPUT=mesh.vert.h -DVAR_NAME=kMeshVertSrc -P embed_js.cmake
#
# Produces a header like:
#   // Auto-generated from mesh.vert — do not edit.
#   #pragma once
#   static const char kMeshVertSrc[] = { 0x0a, ..., 0x00 };

file(READ "${INPUT}" JS_HEX HEX)
get_filename_component(INPUT_NAME "${INPUT}" NAME)

# Emit the JS content as a byte array. Raw string literals ( R"..." ) hit
# MSVC's ~16KB single-literal limit for larger files; a byte array avoids
# that entirely.
string(LENGTH "${JS_HEX}" HEX_LEN)
math(EXPR BYTE_COUNT "${HEX_LEN} / 2")

set(BYTES "")
set(i 0)
while(i LESS HEX_LEN)
    string(SUBSTRING "${JS_HEX}" ${i} 2 B)
    string(APPEND BYTES "0x${B},")
    math(EXPR i "${i} + 2")
endwhile()

file(WRITE "${OUTPUT}"
"// Auto-generated from ${INPUT_NAME} — do not edit.
#pragma once
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4838) // narrowing: byte values >0x7F in signed char[]
#endif
static const char ${VAR_NAME}[] = {
${BYTES}0x00
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
")
