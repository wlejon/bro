add_library(bro_js STATIC runtime.cpp console.cpp timers.cpp dom_bindings.cpp event_dispatch.cpp canvas_bindings.cpp)
target_include_directories(bro_js PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_js PUBLIC qjs bro_dom bro_util bro_canvas)
# QuickJS headers
target_include_directories(bro_js PRIVATE ${CMAKE_SOURCE_DIR}/third_party/quickjs)
