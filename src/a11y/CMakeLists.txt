add_library(bro_a11y STATIC a11y_bridge.cpp)
target_include_directories(bro_a11y PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bro_a11y PUBLIC broa11y::broa11y bro_dom bro_layout)

if(BRO_BUILD_TESTS)
    add_executable(bro_a11y_test tests/test_a11y_bridge.cpp)
    target_include_directories(bro_a11y_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(bro_a11y_test PRIVATE bro_a11y bro_dom bro_layout bro_render bro_util bro_engine)
endif()
