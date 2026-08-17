add_executable(bro-headless main.cpp)
target_link_libraries(bro-headless PRIVATE bro_engine)
if(BRO_WITH_GAMEAI)
    target_link_libraries(bro-headless PRIVATE brogameagent)
endif()
# The compiled half of an app directory (bronze_host/app_module.h) — see the
# matching block for bro.exe in src/CMakeLists.txt. A driver script must be
# able to step a compiled app, so the loader belongs here too.
if(BRO_WITH_BRONZE)
    target_link_libraries(bro-headless PRIVATE bro_bronze_host)
    target_compile_definitions(bro-headless PRIVATE BRO_WITH_BRONZE=1)
    bro_bronze_stage_runtime(bro-headless)
else()
    target_compile_definitions(bro-headless PRIVATE BRO_WITH_BRONZE=0)
endif()
target_include_directories(bro-headless PRIVATE ${bro_SOURCE_DIR}/src)

# QuickJS's JS_CallInternal has a large stack frame; Vue's template compiler
# recurses deeply.  Default 1 MB stack is not enough — use 8 MB.
if(MSVC)
    target_link_options(bro-headless PRIVATE /STACK:8388608)
endif()

if(WIN32)
    target_link_libraries(bro-headless PRIVATE ws2_32)
endif()
