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
# Keep broimage::broimage (which pulls brotensor -> brass) ahead of bro_engine
# (which pulls bronze_runtime_shared) so MSVC does not hit LNK2005 on brass_parallel_for.
target_link_libraries(bro_eye PUBLIC broimage::broimage bro_engine)
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
        add_executable(bro_eye_structure_test tests/test_eye_structure.cpp)
        target_include_directories(bro_eye_structure_test PRIVATE ${bro_SOURCE_DIR}/src)
        # Same cycle pattern as src/scene/CMakeLists.txt: a real SceneGraph pulls
        # in the DOM, whose style code calls back into bro_engine, so bro_engine is
        # named again after bro_dom. Avoids $<LINK_GROUP:RESCAN> (unsupported by
        # Ninja+MSVC and Apple ld64), avoids hardcoding bro_a11y (absent when
        # BRO_WITH_A11Y=OFF), and appends bronze::runtime_shared at the end so
        # CMake places it after broimage -> brotensor -> brass on MSVC,
        # preventing LNK2005 on brass_parallel_for.
        target_link_libraries(bro_eye_structure_test PRIVATE
            bro_eye bro_scene bro_engine bro_dom bro_engine
            bro_render bro_platform bro_util brass skia Vulkan::Vulkan)
        if(TARGET bronze::runtime_shared)
            target_link_libraries(bro_eye_structure_test PRIVATE bronze::runtime_shared)
        endif()
        target_compile_features(bro_eye_structure_test PRIVATE cxx_std_20)
        bro_apply_feature_defs(bro_eye_structure_test)
        if(MSVC)
            target_compile_options(bro_eye_structure_test PRIVATE /MP /permissive- /utf-8)
        endif()
    endif()
endif()
