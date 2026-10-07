add_library(bro_compositor STATIC
    wayland_compositor.cpp
)
target_include_directories(bro_compositor PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_compositor PUBLIC
    bro_render
    bro_util
    brocompositor
)
target_link_libraries(bro_compositor PRIVATE
    brocompositor_api
)
if(BRO_WITH_DMABUF)
    target_link_libraries(bro_compositor PUBLIC brodmabuf::brodmabuf)
endif()
# brocompositor builds its Wayland server role only on Linux with wlroots
# 0.18; without it the compositor reports itself unavailable.
set(_bro_wayland_server 0)
if(TARGET brocompositor_wayland)
    target_link_libraries(bro_compositor PUBLIC brocompositor::wayland)
    set(_bro_wayland_server 1)
else()
    message(STATUS "bro: brocompositor has no Wayland server (needs wlroots 0.18); bro.compositor's server is unavailable")
endif()
target_compile_definitions(bro_compositor PUBLIC BRO_HAVE_WAYLAND_SERVER=${_bro_wayland_server})

# The tests run bro as a Wayland compositor with real clients, so they exist
# only where the Wayland server does.
if(BRO_BUILD_TESTS AND BRO_WITH_COMPOSITOR AND _bro_wayland_server)
    find_package(Vulkan REQUIRED)
    add_executable(bro_nested_compositor_test tests/test_nested_compositor.cpp)
    target_link_libraries(bro_nested_compositor_test PRIVATE
        bro_compositor
        bro_engine
        bro_render
        bro_platform
        bro_util
        skia
        Vulkan::Vulkan
        brocompositor
    )
    if(BRO_WITH_DMABUF)
        target_link_libraries(bro_nested_compositor_test PRIVATE brodmabuf::brodmabuf)
    endif()
    target_include_directories(bro_nested_compositor_test PRIVATE ${bro_SOURCE_DIR}/src)

    add_executable(bro_shell_surfaces_test tests/test_shell_surfaces.cpp)
    target_link_libraries(bro_shell_surfaces_test PRIVATE
        bro_compositor
        bro_render
        bro_util
        brocompositor
    )
    if(BRO_WITH_DMABUF)
        target_link_libraries(bro_shell_surfaces_test PRIVATE brodmabuf::brodmabuf)
    endif()
    target_include_directories(bro_shell_surfaces_test PRIVATE ${bro_SOURCE_DIR}/src)

    add_executable(bro_xwayland_test tests/test_xwayland.cpp)
    target_link_libraries(bro_xwayland_test PRIVATE
        bro_compositor
        bro_render
        bro_util
        brocompositor
    )
    if(BRO_WITH_DMABUF)
        target_link_libraries(bro_xwayland_test PRIVATE brodmabuf::brodmabuf)
    endif()
    target_include_directories(bro_xwayland_test PRIVATE ${bro_SOURCE_DIR}/src)

    add_executable(bro_daily_driver_test tests/test_daily_driver.cpp)
    target_link_libraries(bro_daily_driver_test PRIVATE
        bro_compositor
        bro_render
        bro_util
        brocompositor
    )
    if(BRO_WITH_DMABUF)
        target_link_libraries(bro_daily_driver_test PRIVATE brodmabuf::brodmabuf)
    endif()
    target_include_directories(bro_daily_driver_test PRIVATE ${bro_SOURCE_DIR}/src)
endif()
