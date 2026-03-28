add_library(bro_platform STATIC sdl_window.cpp vulkan_context.cpp event_loop.cpp)
target_include_directories(bro_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_platform PUBLIC SDL3::SDL3-static bro_util)

# Vulkan headers from bundled Khronos Vulkan-Headers (always needed for type definitions)
target_include_directories(bro_platform PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include
)

# Only link Vulkan loader when Skia is available (VulkanContext is used)
if(BRO_USE_SKIA)
    if(Vulkan_FOUND)
        target_link_libraries(bro_platform PUBLIC Vulkan::Vulkan)
    elseif(WIN32)
        target_link_libraries(bro_platform PUBLIC vulkan-1)
    endif()
endif()
