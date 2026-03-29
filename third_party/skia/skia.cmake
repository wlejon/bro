# Skia - imported pre-built static library (required)
#
# Populate third_party/skia/include/ and third_party/skia/lib/ with a Skia
# build before building. Build instructions:
#   cd third_party/skia/src && python3 tools/git-sync-deps
#   bin/gn gen out/Release --args='...'  (see CLAUDE.md)
#   ninja -C out/Release skia

if(WIN32)
    set(_skia_lib "${CMAKE_CURRENT_LIST_DIR}/lib/skia.lib")
else()
    set(_skia_lib "${CMAKE_CURRENT_LIST_DIR}/lib/libskia.a")
endif()

if(NOT EXISTS "${_skia_lib}")
    message(FATAL_ERROR
        "Skia library not found at ${_skia_lib}\n"
        "Place pre-built Skia in third_party/skia/lib/ and third_party/skia/include/")
endif()

add_library(skia STATIC IMPORTED GLOBAL)
set_target_properties(skia PROPERTIES IMPORTED_LOCATION "${_skia_lib}")
# Skia headers use #include <include/core/...> so the include root is the parent
target_include_directories(skia INTERFACE "${CMAKE_CURRENT_LIST_DIR}/src")
