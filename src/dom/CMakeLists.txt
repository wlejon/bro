add_library(bro_dom STATIC document.cpp element.cpp element_geometry.cpp text_node.cpp comment_node.cpp event.cpp event_target.cpp style_proxy.cpp shadow_root.cpp range.cpp selection.cpp)
target_include_directories(bro_dom PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_dom PUBLIC htmlayout gumbo bro_util)
