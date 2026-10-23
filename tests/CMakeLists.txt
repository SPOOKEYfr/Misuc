add_executable(misuc_tests
  test_main.cpp
  test_utils.h
  test_ring.cpp
  test_json.cpp
  test_project.cpp
  test_transport.cpp
  test_dsp.cpp
  test_pitch.cpp
  test_midi.cpp
  test_logger.cpp
  test_undo.cpp
  test_clips.cpp
  test_mixer.cpp
  test_record.cpp
  test_content_end.cpp
  test_wavfile.cpp
  test_export.cpp
  test_audio.cpp
  test_fx.cpp
  test_import.cpp
  test_routing.cpp)

target_link_libraries(misuc_tests PRIVATE misuc_engine)
target_compile_options(misuc_tests PRIVATE)

# The backend-specific live tests are per-platform: PipeWire tests on Linux,
# WASAPI tests on Windows (see MISUC_BACKEND_* definitions in engine/CMakeLists).
if(WIN32)
  target_sources(misuc_tests PRIVATE test_audio_wasapi.cpp)
endif()

add_executable(misuc_headroom ../tools/headroom.cpp)
target_link_libraries(misuc_headroom PRIVATE misuc_engine)

add_test(NAME misuc_tests COMMAND misuc_tests)
add_executable(misuc_flp ../tools/flp_dump.cpp)
target_link_libraries(misuc_flp PRIVATE misuc_engine)
# Linux-only tool install: keep the Windows installer tree flat (bin/ would
# nest a stray misuc_flp.exe next to windeployqt's plugin dirs).
if(NOT WIN32)
  install(TARGETS misuc_flp RUNTIME DESTINATION bin)
endif()

add_executable(misuc_tests_flp test_flp.cpp)
target_link_libraries(misuc_tests_flp PRIVATE misuc_engine)
add_test(NAME misuc_tests_flp COMMAND misuc_tests_flp)
