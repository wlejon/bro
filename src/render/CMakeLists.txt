# --- Render library (OpenGL backend) ---
add_library(bro_render STATIC skia_backend.cpp gl_context.cpp)
target_include_directories(bro_render PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_render PUBLIC bro_platform skia bro_util glad)

find_package(OpenGL REQUIRED)
target_link_libraries(bro_render PUBLIC OpenGL::GL)
