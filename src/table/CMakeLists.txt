add_library(bro_table STATIC
    table_types.h
    table_chunk.h
    table_chunk.cpp
    table_index.h
    table_index.cpp
    table.h
    table.cpp
    query_ir.h
    query_ir.cpp
    query_eval.h
    query_eval.cpp
    query_jit.h
    query_jit.cpp
    query_exec.h
    query_exec.cpp
)

target_include_directories(bro_table PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_link_libraries(bro_table PUBLIC
    brass
)

target_compile_features(bro_table PUBLIC cxx_std_20)

if(BRO_BUILD_TESTS)
    add_executable(bro_table_test
        tests/test_table.cpp
    )
    target_link_libraries(bro_table_test PRIVATE
        bro_table
    )
    set_target_properties(bro_table_test PROPERTIES FOLDER "tests")
endif()
