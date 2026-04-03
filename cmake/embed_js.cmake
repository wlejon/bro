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

file(READ "${INPUT}" JS_CONTENT)
get_filename_component(INPUT_NAME "${INPUT}" NAME)

file(WRITE "${OUTPUT}"
"// Auto-generated from ${INPUT_NAME} — do not edit.\n\
#pragma once\n\
static const char ${VAR_NAME}[] = R\"__JS__(\n\
${JS_CONTENT})__JS__\";\n"
)
