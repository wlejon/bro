add_library(bro_js STATIC
    runtime.cpp console.cpp timers.cpp dom_bindings.cpp event_dispatch.cpp
    canvas_bindings.cpp audio_bindings.cpp storage_bindings.cpp
    webgl2_bindings.cpp
    webgl2_bindings_state.cpp
    webgl2_bindings_buffers.cpp
    webgl2_bindings_shaders.cpp
    webgl2_bindings_textures.cpp
    webgl2_bindings_framebuffers.cpp
    webgl2_bindings_queries.cpp
    image_bindings.cpp
)
target_include_directories(bro_js PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_js PUBLIC qjs bro_dom bro_util bro_canvas bro_audio bro_webgl)
# QuickJS headers
target_include_directories(bro_js PRIVATE ${CMAKE_SOURCE_DIR}/third_party/quickjs)
