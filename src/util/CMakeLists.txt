add_library(bro_util STATIC log.cpp string_utils.cpp time.cpp interrupt.cpp asset_mounts.cpp import_map.cpp object_url.cpp remote_asset.cpp)
target_include_directories(bro_util PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_util PUBLIC bromath::bromath)
target_link_libraries(bro_util PRIVATE qjs)

# The document loader fetches absolute-URL <script>/<link>/module sources.
# libcurl comes in with brokit; without it the cache still works and an
# uncached URL fails with a clear message instead of a mangled path.
if(TARGET libcurl_static)
    target_link_libraries(bro_util PRIVATE libcurl_static)
    target_compile_definitions(bro_util PRIVATE BRO_HAVE_CURL)
endif()
