find_package(unofficial-libwebm CONFIG REQUIRED)
find_package(unofficial-libvpx CONFIG REQUIRED)
find_package(Opus CONFIG REQUIRED)

add_library(bro_video STATIC
    media_backend.cpp
    media_clock.cpp
    webm_demuxer.cpp
    video_decoder.cpp
    audio_decoder.cpp
    yuv_to_rgb.cpp
    rgba_to_yuv.cpp
    webm_encoder.cpp
    gif_encoder.cpp
    video_pipeline.cpp
    media_analysis.cpp
)
target_include_directories(bro_video PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_video
    PUBLIC bro_util
    PRIVATE unofficial::libwebm::libwebm
            unofficial::libvpx::libvpx
            Opus::opus
)

# vcpkg's unofficial-libwebm target exposes headers at include/webm/, but
# libwebm's own headers cross-reference each other via mkvparser/... with
# no prefix. Add the webm/ subdir directly so those resolve.
get_target_property(_webm_inc unofficial::libwebm::libwebm INTERFACE_INCLUDE_DIRECTORIES)
if(_webm_inc)
    foreach(_d IN LISTS _webm_inc)
        if(EXISTS "${_d}/webm")
            target_include_directories(bro_video PRIVATE "${_d}/webm")
        endif()
    endforeach()
endif()

# Smoke test: decode a WebM and dump what we saw. Not linked into bro.
add_executable(bro_videoinspect tools/video_inspect.cpp)
target_link_libraries(bro_videoinspect PRIVATE bro_video)

# Smoke test: encode a synthetic gradient to WebM, decode it back, and
# report frame counts / sizes / first-pixel sanity check. Not linked into bro.
add_executable(bro_videoencodetest tools/video_encode_test.cpp)
target_link_libraries(bro_videoencodetest PRIVATE bro_video)

# Smoke test: the media backend registry — precedence, fallthrough when a
# backend can't decode, and a non-WebM source reaching the pipeline. Uses fake
# backends, since the built-in can't demonstrate any of that alone.
add_executable(bro_mediabackendtest tools/media_backend_test.cpp)
target_link_libraries(bro_mediabackendtest PRIVATE bro_video)
