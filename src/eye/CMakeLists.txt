add_library(bro_eye STATIC
    eye_measure.cpp
    eye_compare.cpp
    eye_compose.cpp
    eye_motion.cpp
    eye_motion_visual.cpp
    eye_layout.cpp
    eye_audit.cpp
    eye_visualize.cpp
    eye_visualize_taste.cpp
    eye_fit.cpp
)

if(BRO_WITH_3D)
    target_sources(bro_eye PRIVATE
        eye_scene.cpp
        eye_silhouette.cpp
        eye_form.cpp
        eye_probe.cpp
        eye_clearance.cpp
        eye_structure.cpp
        eye_structure_collect.cpp
        eye_structure_floors.cpp
        eye_structure_islands.cpp
        eye_structure_walk.cpp
        eye_closeup.cpp
        eye_lookdev.cpp
        eye_trace.cpp
        eye_diagnose.cpp
        eye_conformance.cpp
        eye_scene_form.cpp
    )
endif()

target_include_directories(bro_eye PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_eye PUBLIC bro_engine broimage::broimage)
target_compile_features(bro_eye PUBLIC cxx_std_20)
bro_apply_feature_defs(bro_eye)

if(MSVC)
    target_compile_options(bro_eye PRIVATE /MP /permissive- /utf-8)
endif()

if(BRO_BUILD_TESTS)
    add_executable(bro_eye_test
        tests/test_eye.cpp
        eye_measure.cpp
        eye_compare.cpp
        eye_compose.cpp
        eye_motion.cpp
        eye_motion_visual.cpp
        eye_visualize.cpp
        eye_visualize_taste.cpp
        eye_fit.cpp
    )
    target_include_directories(bro_eye_test PRIVATE ${bro_SOURCE_DIR}/src)
    target_link_libraries(bro_eye_test PRIVATE broimage::broimage)
    if(BRO_WITH_3D AND TARGET bromesh)
        target_sources(bro_eye_test PRIVATE eye_silhouette.cpp eye_form.cpp)
        target_link_libraries(bro_eye_test PRIVATE bromesh)
        target_compile_definitions(bro_eye_test PRIVATE BRO_EYE_TEST_WITH_FORM=1)
    endif()
    target_compile_features(bro_eye_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(bro_eye_test PRIVATE /MP /permissive- /utf-8)
    endif()
    if(BRO_WITH_3D)
        find_package(Vulkan REQUIRED)
        add_executable(bro_eye_structure_test
            tests/test_eye_structure.cpp
            eye_measure.cpp
            eye_structure.cpp
            eye_structure_collect.cpp
            eye_structure_floors.cpp
            eye_structure_islands.cpp
            eye_structure_walk.cpp
        )
        target_include_directories(bro_eye_structure_test PRIVATE ${bro_SOURCE_DIR}/src)
        # Compile the eye_structure sources directly (like bro_eye_test above) and use
        # the exact link order of bro_sdf_gpu_tracer_test (src/scene/CMakeLists.txt):
        # avoiding bro_eye keeps broimage -> brotensor -> brass ahead of
        # brocompositor_api -> bronze_runtime_shared so MSVC does not hit LNK2005 on
        # brass_parallel_for.
        target_link_libraries(bro_eye_structure_test PRIVATE
            bro_scene bro_engine bro_dom bro_engine
            bro_render bro_platform bro_util bromesh bromath brass skia Vulkan::Vulkan)
        target_compile_features(bro_eye_structure_test PRIVATE cxx_std_20)
        bro_apply_feature_defs(bro_eye_structure_test)
        if(MSVC)
            target_compile_options(bro_eye_structure_test PRIVATE /MP /permissive- /utf-8)
        endif()
    endif()
endif()
