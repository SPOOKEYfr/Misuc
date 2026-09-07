find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

add_executable(misuc_app
  src/main.cpp
  src/Theme.cpp
  src/theme/ThemeEngine.cpp
  src/MainWindow.cpp
  src/TransportBar.cpp
  src/MeterWidget.cpp
  src/AudioSettingsPage.cpp
  src/ProjectPage.cpp
  src/StatusPanel.cpp
  src/PianoRoll.cpp
  src/EditorPage.cpp)

target_include_directories(misuc_app PRIVATE src)

target_link_libraries(misuc_app PRIVATE
  misuc_engine
  Qt6::Widgets)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_definitions(misuc_app PRIVATE MISUC_DEBUG)
endif()