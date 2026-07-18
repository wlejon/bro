# --- Bidi (UAX #9) ---
#
# Its own target for one reason: bidi.cpp is the only file in bro that includes
# an ICU header, and ICU's headers only declare the symbols the vendored subset
# actually exports when a specific set of renaming macros is defined. Confining
# both the include path and those macros to a one-file target keeps them out of
# every other translation unit, where they would be inert at best and confusing
# at worst.
add_library(bro_bidi STATIC bidi.cpp)
target_include_directories(bro_bidi PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_bidi PUBLIC bro_util)
if(BRO_WITH_TEXT_SHAPING)
    target_link_libraries(bro_bidi PRIVATE skia_icu_bidi_api)
    target_compile_definitions(bro_bidi PRIVATE BRO_WITH_ICU_BIDI=1)
else()
    target_compile_definitions(bro_bidi PRIVATE BRO_WITH_ICU_BIDI=0)
endif()

# --- Render library (OpenGL backend) ---
add_library(bro_render STATIC
    skia_backend.cpp
    gl_context.cpp
    raster_renderer.cpp
    font_fallback.cpp
    font_family.cpp
    shaped_run.cpp
    filter_chain.cpp
    recording_renderer.cpp
    command_replayer.cpp
    image_cache.cpp
)
target_include_directories(bro_render PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_render PUBLIC bro_platform skia bro_svg bro_util bro_bidi glad broimage::broimage)

find_package(OpenGL REQUIRED)
target_link_libraries(bro_render PUBLIC OpenGL::GL)

# On Linux, bro doesn't reference libGL symbols directly (glad uses dlsym, SDL
# loads GL), so --as-needed drops the libGL.so DT_NEEDED entry. Skia's
# GrGLMakeNativeInterface() relies on dlsym(RTLD_DEFAULT, ...) finding GL
# entry points, which fails when libGL is unloaded. Force the link.
if(UNIX AND NOT APPLE)
    target_link_options(bro_render INTERFACE
        "LINKER:--no-as-needed,-lGL,--as-needed")
endif()
