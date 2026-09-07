add_executable(misuc_tests
  test_main.cpp
  test_utils.h
  test_ring.cpp
  test_json.cpp
  test_transport.cpp
  test_dsp.cpp
  test_midi.cpp
  test_logger.cpp
  test_undo.cpp
  test_audio.cpp)

target_link_libraries(misuc_tests PRIVATE misuc_engine)
target_compile_options(misuc_tests PRIVATE)

add_test(NAME misuc_tests COMMAND misuc_tests)