# Skia SVG module — compiled from source against the pre-built Skia core library.
#
# This builds modules/svg, modules/skshaper (primitive text only), and
# modules/skresources as a single static library target: skia_svg.
#
# Requires the "skia" imported target from skia.cmake.

set(_skia_src "${CMAKE_CURRENT_LIST_DIR}/src")

# --- Collect sources ---

file(GLOB _svg_sources      "${_skia_src}/modules/svg/src/*.cpp")
file(GLOB _skresources_src  "${_skia_src}/modules/skresources/src/*.cpp")

set(_skshaper_sources
    "${_skia_src}/modules/skshaper/src/SkShaper.cpp"
    "${_skia_src}/modules/skshaper/src/SkShaper_primitive.cpp"
    "${_skia_src}/modules/skshaper/src/SkShaper_factory.cpp"
)

# Skia's XML/DOM parser (SkDOM + SkXMLParser) and expat — these may not be
# included in the pre-built skia.lib (e.g. Windows builds without system expat).
# Compiling them here is harmless if they're already in skia.lib (linker dedupes).
set(_xml_sources
    "${_skia_src}/src/xml/SkDOM.cpp"
    "${_skia_src}/src/xml/SkXMLParser.cpp"
    "${_skia_src}/src/xml/SkXMLWriter.cpp"
)
set(_expat_sources
    "${_skia_src}/third_party/externals/expat/expat/lib/xmlparse.c"
    "${_skia_src}/third_party/externals/expat/expat/lib/xmlrole.c"
    "${_skia_src}/third_party/externals/expat/expat/lib/xmltok.c"
)

add_library(skia_svg STATIC
    ${_svg_sources}
    ${_skresources_src}
    ${_skshaper_sources}
    ${_xml_sources}
    ${_expat_sources}
)

# --- Include paths ---
# Skia source root (for include/core/..., src/..., modules/.../include/...)
target_include_directories(skia_svg PRIVATE "${_skia_src}")

# Module public headers — consumers of skia_svg get these too
target_include_directories(skia_svg PUBLIC
    "${_skia_src}/modules/svg/include"
    "${_skia_src}/modules/skshaper/include"
    "${_skia_src}/modules/skresources/include"
)

# Expat headers (bundled in Skia's third_party)
target_include_directories(skia_svg PRIVATE
    "${_skia_src}/third_party/externals/expat/expat/lib"
    "${_skia_src}/third_party/expat/include/expat_config"
)

# --- Defines ---
target_compile_definitions(skia_svg PUBLIC
    SK_ENABLE_SVG
    SK_SHAPER_PRIMITIVE_AVAILABLE
)
target_compile_definitions(skia_svg PRIVATE
    XML_STATIC
    SKSHAPER_IMPLEMENTATION=1
)

# --- Link pre-built Skia core ---
target_link_libraries(skia_svg PUBLIC skia)

# MSVC: disable noisy warnings from Skia source
if(MSVC)
    target_compile_options(skia_svg PRIVATE /W0)
else()
    target_compile_options(skia_svg PRIVATE -w)
endif()
