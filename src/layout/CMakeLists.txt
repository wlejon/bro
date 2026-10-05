add_library(bro_layout STATIC computed_style.cpp control_text.cpp css_shadow.cpp form_control.cpp form_validation.cpp draw_traversal.cpp draw_traversal_background.cpp draw_traversal_border_image.cpp draw_traversal_borders.cpp draw_traversal_color.cpp draw_traversal_css.cpp draw_traversal_element.cpp draw_traversal_image.cpp draw_traversal_text.cpp draw_traversal_top_layer.cpp top_layer_hit.cpp el_input.cpp el_input_edit.cpp el_input_paint.cpp el_textarea.cpp el_select.cpp el_svg.cpp el_video.cpp el_terminal.cpp selection_geometry.cpp svg_geometry.cpp svg_common.cpp svg_paint.cpp svg_text.cpp)
target_include_directories(bro_layout PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_layout PUBLIC htmlayout bro_render bro_svg bro_util broimage::broimage broaudio bro_dom)

# el_video.cpp guards its VideoPipeline usage behind #if BRO_WITH_VIDEO, so the
# core layout lib only links the (vcpkg-backed) bro_video decoder when video is
# built. Stamp the feature flags so those guards resolve.
bro_apply_feature_defs(bro_layout)
if(BRO_WITH_VIDEO)
    target_link_libraries(bro_layout PUBLIC bro_video)
endif()
# el_terminal.cpp: the same arrangement for <terminal> and bro_terminal.
if(BRO_WITH_TERMINAL)
    target_sources(bro_layout PRIVATE el_terminal_layer.cpp el_terminal_mouse.cpp el_terminal_view.cpp
                                      el_terminal_events.cpp el_terminal_theme.cpp)
    target_link_libraries(bro_layout PUBLIC bro_terminal)
else()
    target_sources(bro_layout PRIVATE el_terminal_off.cpp)
endif()
