add_library(bro_platform STATIC sdl_window.cpp vulkan_context.cpp event_loop.cpp)
target_include_directories(bro_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_platform PUBLIC SDL3::SDL3-static bro_util)

# Vulkan headers from bundled Khronos Vulkan-Headers
target_include_directories(bro_platform PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include
)

# Vulkan loader
if(Vulkan_FOUND)
    target_link_libraries(bro_platform PUBLIC Vulkan::Vulkan)
else()
    # Use generated import lib for vulkan-1.dll
    target_link_libraries(bro_platform PUBLIC
        ${CMAKE_SOURCE_DIR}/third_party/libvulkan-1.a
    )
endif()
