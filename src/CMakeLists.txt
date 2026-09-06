add_subdirectory(util)
add_subdirectory(tile)
add_subdirectory(audio_inference)
add_subdirectory(platform)
add_subdirectory(svg)
add_subdirectory(render)
add_subdirectory(js)
add_subdirectory(dom)
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

# bronze AOT host layer — strictly opt-in; the default build never sees it
# (see the option's comment in the top-level CMakeLists.txt).
if(BRO_WITH_BRONZE)
    add_subdirectory(bronze_host)
endif()

# The three stock executables. A host application that embeds bro (links
# bro_engine and supplies its own main) turns these off with
# -DBRO_BUILD_EXECUTABLES=OFF: it wants the libraries, not a second bro.exe
# in its build tree. The libraries above are always built; bro's own C++
# tests and smoke tools (tile/, video/) sit behind BRO_BUILD_TESTS by the
# same rule.
if(BRO_BUILD_EXECUTABLES)
    add_subdirectory(headless)
    add_subdirectory(server)

    # WIN32 = /SUBSYSTEM:WINDOWS — no console window on double-click.
    # main.cpp calls AttachConsole() at startup so stdout still shows when
    # launched from a terminal. Ignored on non-Windows platforms.
    add_executable(bro WIN32 main.cpp)
    target_link_libraries(bro PRIVATE bro_engine)
    if(BRO_WITH_GAMEAI)
        target_link_libraries(bro PRIVATE brogameagent)
    endif()
    # The compiled half of an app directory (bronze_host/app_module.h). bro.exe
    # opens an interpreted app and a compiled one with the same binary, so it
    # needs the loader whenever the host layer is configured at all — the
    # decision is per-FOLDER at runtime, not per-build.
    if(BRO_WITH_BRONZE)
        target_link_libraries(bro PRIVATE bro_bronze_host)
        target_compile_definitions(bro PRIVATE BRO_WITH_BRONZE=1)
        bro_bronze_stage_runtime(bro)
    else()
        target_compile_definitions(bro PRIVATE BRO_WITH_BRONZE=0)
    endif()
    target_include_directories(bro PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # QuickJS's JS_CallInternal has a large stack frame; deep JS recursion
    # (e.g. Vue template compiler) needs more than the default 1 MB stack.
    if(MSVC)
        target_link_options(bro PRIVATE /STACK:8388608)
        # GUI subsystem but keep int main() as the entry point.
        target_link_options(bro PRIVATE /ENTRY:mainCRTStartup)
    endif()

    if(WIN32)
        target_link_libraries(bro PRIVATE ws2_32)
    endif()

    # Embed a Windows application manifest into every executable: opts into
    # Common-Controls v6 (themed native dialogs instead of Windows-95 grey)
    # and declares Windows 10/11 support. MSVC's manifest tool merges it with
    # the linker-generated manifest.
    if(MSVC)
        foreach(_bro_target bro bro-headless bro-server)
            target_sources(${_bro_target} PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/bro.manifest)
        endforeach()
    endif()
endif()
