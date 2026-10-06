add_library(bro_platform STATIC sdl_window.cpp sdl_runtime.cpp event_loop.cpp
                                clipboard.cpp dialogs.cpp
                                desktop_platform.cpp desktop_bell.cpp
                                desktop_progress.cpp desktop_notifications.cpp
                                desktop_tray.cpp desktop_hotkeys.cpp
                                desktop_single_instance.cpp)
target_include_directories(bro_platform PUBLIC ${bro_SOURCE_DIR}/src)

# SDL3 — use the umbrella target (resolves to shared or static automatically)
if(TARGET SDL3::SDL3)
    target_link_libraries(bro_platform PUBLIC SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
    target_link_libraries(bro_platform PUBLIC SDL3::SDL3-static)
else()
    message(FATAL_ERROR "No SDL3 target found")
endif()

find_package(Vulkan REQUIRED)
target_link_libraries(bro_platform PUBLIC bro_util Vulkan::Headers)
target_link_libraries(bro_platform PRIVATE broimage::broimage ${CMAKE_DL_LIBS})
