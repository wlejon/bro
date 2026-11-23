add_library(bro_table STATIC
    table_types.h
    table_chunk.h
    table_chunk.cpp
    table_index.h
    table_index.cpp
    table_range_index.h
    table_range_index.cpp
    table_spatial_index.cpp
    table_io.h
    table_io_internal.h
    table_io_index.cpp
    table_io.cpp
    table_reactive.h
    table_reactive.cpp
    table.h
    table.cpp
    query_ir.h
    query_ir.cpp
    query_eval.h
    query_eval.cpp
    query_jit_emit.h
    query_jit_agg.h
    query_jit_agg.cpp
    query_jit.h
    query_jit.cpp
    query_jit_simd.cpp
    query_groupby.h
    query_groupby.cpp
    query_materialize.h
    query_materialize.cpp
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

    add_executable(bro_table_advanced_test
        tests/test_table_advanced.cpp
    )
    target_link_libraries(bro_table_advanced_test PRIVATE
        bro_table
    )
    set_target_properties(bro_table_advanced_test PROPERTIES FOLDER "tests")

    add_executable(bro_table_production_test
        tests/test_table_production.cpp
    )
    target_link_libraries(bro_table_production_test PRIVATE
        bro_table
    )
    set_target_properties(bro_table_production_test PROPERTIES FOLDER "tests")

    add_executable(bro_table_bench
        tests/bench_table.cpp
    )
    target_link_libraries(bro_table_bench PRIVATE
        bro_table
    )
    set_target_properties(bro_table_bench PROPERTIES FOLDER "tests")
endif()
