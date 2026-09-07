add_library(misuc_engine STATIC
  src/core/Json.cpp
  src/core/Logger.cpp
  src/audio/PipeWireBackend.cpp
  src/audio/AudioEngine.cpp
  src/audio/WavFile.cpp
  src/project/Session.cpp)

target_include_directories(misuc_engine PUBLIC)
target_include_directories(misuc_engine SYSTEM PUBLIC include)

target_compile_options(misuc_engine PRIVATE -fvisibility=hidden)

target_link_libraries(misuc_engine PUBLIC
  PkgConfig::PIPEWIRE
  PkgConfig::ALSA
  PkgConfig::SNDFILE)

set_target_properties(misuc_engine PROPERTIES
  POSITION_INDEPENDENT_CODE ON)