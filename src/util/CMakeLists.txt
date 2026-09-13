add_library(bro_util STATIC log.cpp string_utils.cpp time.cpp interrupt.cpp asset_mounts.cpp asset_path.cpp object_url.cpp remote_asset.cpp user_dirs.cpp storage_file.cpp)
target_include_directories(bro_util PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_util PUBLIC bromath::bromath)

# The document loader fetches absolute-URL <script>/<link>/module sources.
# libcurl comes in with brokit; without it the cache still works and an
# uncached URL fails with a clear message instead of a mangled path.
if(TARGET libcurl_static)
    target_link_libraries(bro_util PRIVATE libcurl_static)
    target_compile_definitions(bro_util PRIVATE BRO_HAVE_CURL)
endif()
