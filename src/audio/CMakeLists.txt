# bro_audio is now a thin interface library that re-exports broaudio.
# The header at audio/audio_engine.h provides namespace aliases so existing
# code (#include "audio/audio_engine.h", bro::audio::AudioEngine) still works.
add_library(bro_audio INTERFACE)
target_include_directories(bro_audio INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_audio INTERFACE broaudio)
