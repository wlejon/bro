# bro/src/c_abi/CMakeLists.txt — Direct C-ABI Layer
add_library(bro_c_abi STATIC
    bro_math_c_abi.cpp
)

target_include_directories(bro_c_abi PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${bro_SOURCE_DIR}/include
    ${bro_SOURCE_DIR}/src
    ${bro_SOURCE_DIR}/third_party/bromath/include
)

target_link_libraries(bro_c_abi PUBLIC bromath)
target_compile_features(bro_c_abi PUBLIC cxx_std_20)
