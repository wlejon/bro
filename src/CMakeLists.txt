# SDL is private to src/platform: every target below gets the fence first on
# its include path (src/platform removes it for itself).
include(${bro_SOURCE_DIR}/cmake/sdl_fence.cmake)
bro_sdl_fence_dir(BRO_SDL_FENCE_DIR)
include_directories(BEFORE "${BRO_SDL_FENCE_DIR}")

add_subdirectory(util)
add_subdirectory(tile)
add_subdirectory(tilecache)
add_subdirectory(audio_inference)
add_subdirectory(platform)
add_subdirectory(svg)
add_subdirectory(render)
add_subdirectory(dom)
if(BRO_WITH_TERMINAL)
    add_subdirectory(terminal)
endif()
if(BRO_WITH_A11Y)
    add_subdirectory(a11y)
endif()
if(BRO_WITH_COMPOSITOR)
    add_subdirectory(compositor)
endif()
add_subdirectory(layout)
add_subdirectory(canvas)
add_subdirectory(webgl)
if(BRO_WITH_PHYSICS)
    add_subdirectory(physics)
endif()
if(BRO_WITH_3D)
    add_subdirectory(scene)
endif()
if(BRO_WITH_NET)
    add_subdirectory(net)
endif()
add_subdirectory(steam)
if(BRO_WITH_VIDEO)
    add_subdirectory(video)
endif()
add_subdirectory(engine)

# bronze JavaScript runtime host layer.
add_subdirectory(bronze_host)
add_subdirectory(ctl)

# libbro (the engine as one shared library) and the executables that call
# into it: bro, bro-headless, bro-server and bro-app-stub. A host application
# that embeds bro (links the engine and supplies its own main) turns the
# executables off with -DBRO_BUILD_EXECUTABLES=OFF: it wants the libraries,
# not a second bro.exe in its build tree. The libraries are always built;
# bro's own C++ tests and smoke tools (tile/, video/) sit behind
# BRO_BUILD_TESTS by the same rule.
add_subdirectory(runtime)
