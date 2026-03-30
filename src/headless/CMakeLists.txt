add_library(bro_headless STATIC headless.cpp)
target_include_directories(bro_headless PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_headless PUBLIC bro_js bro_dom bro_layout bro_render bro_canvas bro_audio bro_util)

add_executable(bro-headless main.cpp)
target_link_libraries(bro-headless PRIVATE bro_headless bro_engine)
target_include_directories(bro-headless PRIVATE ${CMAKE_SOURCE_DIR}/src)

# QuickJS's JS_CallInternal has a large stack frame; Vue's template compiler
# recurses deeply.  Default 1 MB stack is not enough — use 8 MB.
if(MSVC)
    target_link_options(bro-headless PRIVATE /STACK:8388608)
endif()

if(WIN32)
    target_link_libraries(bro-headless PRIVATE ws2_32)
endif()
