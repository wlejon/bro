add_library(bro_layout STATIC control_text.cpp draw_traversal.cpp el_input.cpp el_textarea.cpp el_select.cpp el_svg.cpp el_video.cpp selection_geometry.cpp svg_geometry.cpp svg_common.cpp svg_paint.cpp svg_text.cpp)
target_include_directories(bro_layout PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_layout PUBLIC htmlayout bro_render bro_svg bro_util broimage::broimage broaudio bro_dom)

# el_video.cpp guards its VideoPipeline usage behind #if BRO_WITH_VIDEO, so the
# core layout lib only links the (vcpkg-backed) bro_video decoder when video is
# built. Stamp the feature flags so those guards resolve.
bro_apply_feature_defs(bro_layout)
if(BRO_WITH_VIDEO)
    target_link_libraries(bro_layout PUBLIC bro_video)
endif()
