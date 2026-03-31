add_library(bro_dom STATIC document.cpp element.cpp text_node.cpp comment_node.cpp event.cpp style_proxy.cpp shadow_root.cpp)
target_include_directories(bro_dom PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_dom PUBLIC htmlayout gumbo bro_util)
