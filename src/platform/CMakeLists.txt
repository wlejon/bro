add_library(bro_platform STATIC sdl_window.cpp event_loop.cpp)
target_include_directories(bro_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)

# SDL3 — use the umbrella target (resolves to shared or static automatically)
if(TARGET SDL3::SDL3)
    target_link_libraries(bro_platform PUBLIC SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
    target_link_libraries(bro_platform PUBLIC SDL3::SDL3-static)
else()
    message(FATAL_ERROR "No SDL3 target found")
endif()

target_link_libraries(bro_platform PUBLIC bro_util glad)
target_link_libraries(bro_platform PRIVATE stb_image)
