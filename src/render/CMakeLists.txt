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
target_link_libraries(bro_render PUBLIC bro_platform skia bro_svg bro_util glad broimage::broimage)

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
