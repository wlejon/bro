# The platform layer: backend-neutral interfaces (window.h, event_loop.h,
# keys.h, ...) and the backends behind them. See README.md.

# SDL3 is private: only src/platform compiles against it. Every other bro
# target has the SDL fence (cmake/sdl_fence.cmake) first on its include path;
# this directory takes it off, before any target here copies the directory's
# include path.
get_property(_bro_platform_dirs DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
list(REMOVE_ITEM _bro_platform_dirs "${BRO_SDL_FENCE_DIR}")
set_property(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "${_bro_platform_dirs}")

add_library(bro_platform STATIC
    # interfaces and policy
    window_system.cpp keys.cpp dialogs.cpp evdev_keymap.cpp
    # SDL window system
    sdl/sdl_window_system.cpp sdl/sdl_window.cpp sdl/sdl_runtime.cpp
    sdl/sdl_event_loop.cpp sdl/sdl_clipboard.cpp sdl/sdl_dialogs.cpp
    # DRM window system (the KMS/libinput halves are drm_seat / drm_input)
    drm_window_system.cpp
    # desktop integration
    desktop_platform.cpp desktop_bell.cpp desktop_progress.cpp
    desktop_notifications.cpp desktop_tray.cpp desktop_hotkeys.cpp
    desktop_single_instance.cpp control_socket.cpp)
target_include_directories(bro_platform PUBLIC ${bro_SOURCE_DIR}/src)
if(TARGET SDL3::SDL3)
    target_link_libraries(bro_platform PRIVATE SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
    target_link_libraries(bro_platform PRIVATE SDL3::SDL3-static)
else()
    message(FATAL_ERROR "No SDL3 target found")
endif()
if(WIN32)
    # SetCurrentProcessExplicitAppUserModelID (the window's app id).
    target_link_libraries(bro_platform PRIVATE shell32)
    # The agent control socket (AF_UNIX through Winsock).
    target_link_libraries(bro_platform PRIVATE ws2_32)
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

# The Wayland window system (wayland/): a Wayland client of its own through
# browl, picked over SDL's Wayland backend when $WAYLAND_DISPLAY is set
# (selectDesktopWindowSystem). Linux with browl only; elsewhere
# waylandWindowSystem() reports it absent.
set(_bro_have_wayland 0)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND BRO_WITH_WL AND TARGET browl)
    target_sources(bro_platform PRIVATE
        wayland/wayland_window_system.cpp wayland/wayland_window.cpp
        wayland/wayland_event_loop.cpp wayland/wayland_keys.cpp wayland/portal_theme.cpp)
    # Vulkan::Vulkan for vkGetInstanceProcAddr (vkCreateWaylandSurfaceKHR).
    target_link_libraries(bro_platform PRIVATE browl Vulkan::Vulkan)
    set(_bro_have_wayland 1)
endif()
target_compile_definitions(bro_platform PRIVATE BRO_HAVE_WAYLAND_BACKEND=${_bro_have_wayland})

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
