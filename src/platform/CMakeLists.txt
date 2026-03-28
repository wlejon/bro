add_library(bro_platform STATIC sdl_window.cpp vulkan_context.cpp event_loop.cpp)
target_include_directories(bro_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)

# SDL3 — use the umbrella target (resolves to shared or static automatically)
if(TARGET SDL3::SDL3)
    target_link_libraries(bro_platform PUBLIC SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
    target_link_libraries(bro_platform PUBLIC SDL3::SDL3-static)
else()
    message(FATAL_ERROR "No SDL3 target found")
endif()

target_link_libraries(bro_platform PUBLIC bro_util)

# Vulkan headers (always needed for type definitions)
if(TARGET Vulkan::Headers)
    target_link_libraries(bro_platform PUBLIC Vulkan::Headers)
else()
    target_include_directories(bro_platform PUBLIC
        ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include
    )
endif()

# Vulkan loader (only when Skia is available)
if(BRO_USE_SKIA)
    if(TARGET Vulkan::Vulkan)
        target_link_libraries(bro_platform PUBLIC Vulkan::Vulkan)
    elseif(WIN32)
        target_link_libraries(bro_platform PUBLIC vulkan-1)
    endif()
endif()
