# Skia - imported pre-built static library
#
# Populate third_party/skia/include/ and third_party/skia/lib/ with a Skia
# build before enabling this. Until then, we build with BRO_USE_SKIA=OFF
# and the renderer uses a software stub.

option(BRO_USE_SKIA "Link against pre-built Skia" OFF)

if(BRO_USE_SKIA)
    add_library(skia STATIC IMPORTED GLOBAL)
    set_target_properties(skia PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/lib/libskia.a"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/include"
    )
    target_link_libraries(skia INTERFACE Vulkan::Vulkan)
else()
    add_library(skia INTERFACE)
endif()
