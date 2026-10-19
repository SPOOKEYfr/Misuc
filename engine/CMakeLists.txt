add_library(misuc_engine STATIC
  src/core/Json.cpp
  src/core/Logger.cpp
  src/core/ProjectFile.cpp
  src/audio/AudioEngine.cpp
  src/audio/WavFile.cpp
  src/fx/Lv2Host.cpp
  src/project/Session.cpp
  src/flp/FlpReader.cpp
  src/flp/FlpImport.cpp)

target_include_directories(misuc_engine PUBLIC)
target_include_directories(misuc_engine SYSTEM PUBLIC include)

# Platform audio backend: WASAPI on Windows, PipeWire elsewhere.
if(WIN32)
  target_sources(misuc_engine PRIVATE src/audio/WASAPIBackend.cpp)
  target_compile_definitions(misuc_engine PUBLIC MISUC_BACKEND_WASAPI=1)
  target_link_libraries(misuc_engine PUBLIC ole32 uuid avrt)
else()
  target_sources(misuc_engine PRIVATE src/audio/PipeWireBackend.cpp)
  target_compile_definitions(misuc_engine PUBLIC MISUC_BACKEND_PIPEWIRE=1)
  target_link_libraries(misuc_engine PUBLIC PkgConfig::PIPEWIRE PkgConfig::ALSA)
endif()

if(MSVC)
  target_compile_options(misuc_engine PRIVATE /W4 /EHsc /utf-8 /permissive-)
else()
  target_compile_options(misuc_engine PRIVATE -fvisibility=hidden)
endif()

find_package(ZLIB REQUIRED)

target_link_libraries(misuc_engine PUBLIC
  ${MISUC_SNDFILE_TARGET}
  ZLIB::ZLIB)

if(LILV_FOUND AND LV2_FOUND)
  target_compile_definitions(misuc_engine PUBLIC MISUC_LV2=1)
  target_link_libraries(misuc_engine PUBLIC PkgConfig::LILV PkgConfig::LV2)
endif()

set_target_properties(misuc_engine PROPERTIES
  POSITION_INDEPENDENT_CODE ON)
