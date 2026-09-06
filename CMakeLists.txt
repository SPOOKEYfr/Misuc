cmake_minimum_required(VERSION 3.20)

project(misuc
  VERSION 0.1.0
  DESCRIPTION "Misuc - Professional Music Production for Linux"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MISUC_BUILD_TESTS "Build unit tests" ON)
option(MISUC_BUILD_UI   "Build the Qt user interface" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3")
add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(ALSA     REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(SNDFILE  REQUIRED IMPORTED_TARGET sndfile)

add_subdirectory(engine)

if(MISUC_BUILD_UI)
  add_subdirectory(ui)
endif()

if(MISUC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()