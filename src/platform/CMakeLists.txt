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

# DRM input reads libinput through udev; without both it reports itself
# unavailable (DrmInputPlatform::init returns false).
set(_bro_have_libinput 0)
if(BRO_WITH_SEAT)
    target_sources(bro_platform PRIVATE drm_seat.cpp drm_input.cpp)
    target_link_libraries(bro_platform PUBLIC broseat)
    if(UNIX AND NOT APPLE)
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(LIBINPUT QUIET IMPORTED_TARGET libinput)
            pkg_check_modules(LIBUDEV QUIET IMPORTED_TARGET libudev)
            if(TARGET PkgConfig::LIBINPUT AND TARGET PkgConfig::LIBUDEV)
                target_link_libraries(bro_platform PRIVATE PkgConfig::LIBINPUT PkgConfig::LIBUDEV)
                set(_bro_have_libinput 1)
            else()
                message(STATUS "bro: libinput or libudev not found; DRM input is unavailable")
            endif()
        endif()
    endif()
endif()
target_compile_definitions(bro_platform PUBLIC BRO_HAVE_LIBINPUT=${_bro_have_libinput})

if(BRO_BUILD_TESTS AND BRO_WITH_SEAT AND BRO_WITH_DMABUF)
    add_executable(bro_drm_screen_test tests/test_drm_screen.cpp)
    target_link_libraries(bro_drm_screen_test PRIVATE
        bro_platform
        bro_render
        bro_util
        broseat
        brodmabuf::brodmabuf
        Vulkan::Vulkan
    )
    target_include_directories(bro_drm_screen_test PRIVATE ${bro_SOURCE_DIR}/src)
endif()
