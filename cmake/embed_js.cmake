# embed_js.cmake — Convert a .js file into a C++ header with the content as a raw string.
#
# Usage (from add_custom_command):
#   cmake -DINPUT=abort.js -DOUTPUT=abort.js.h -DVAR_NAME=js_abort -P embed_js.cmake
#
# Produces a header like:
#   // Auto-generated from abort.js — do not edit.
#   #pragma once
#   static const char js_abort[] = R"__JS__(
#   ...file contents...
#   )__JS__";

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
static const char ${VAR_NAME}[] = {
${BYTES}0x00
};
")
