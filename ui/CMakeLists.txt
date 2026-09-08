find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

add_executable(misuc_app
  src/main.cpp
  src/Theme.cpp
  src/theme/ThemeEngine.cpp
  src/theme/ThemePage.cpp
  src/branding/Logo.cpp
  src/branding/AboutDialog.cpp
  src/branding/Icons.cpp
  src/HeaderBar.cpp
  src/HintSystem.cpp
  src/MainWindow.cpp
  src/TransportBar.cpp
  src/MeterWidget.cpp
  src/AudioSettingsPage.cpp
  src/ProjectPage.cpp
  src/StatusPanel.cpp
  src/PianoRoll.cpp
  src/EditorPage.cpp
  src/mixer/Knob.cpp
  src/mixer/Fader.cpp
  src/mixer/VerticalMeter.cpp
  src/mixer/ChannelStrip.cpp
  src/mixer/MixerPage.cpp
  src/commands/MisucCommandRegistry.cpp
  src/tooltip/MisucTooltip.cpp
  src/playlist/Playlist.cpp
  src/channel/ChannelRack.cpp
  src/browser/Browser.cpp
  src/inspector/Inspector.cpp
  src/project/DemoProject.cpp
  src/workspace/WorkspaceManager.cpp)

qt_add_resources(misuc_app MISUC_ASSETS
  PREFIX "/"
  ${CMAKE_CURRENT_SOURCE_DIR}/resources.qrc)

target_include_directories(misuc_app PRIVATE src)

target_link_libraries(misuc_app PRIVATE
  misuc_engine
  Qt6::Widgets)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_definitions(misuc_app PRIVATE MISUC_DEBUG)
endif()

# ---------------------------------------------------------------------------
# Linux desktop integration: install the app, a real .desktop entry, and the
# hicolor icon set rendered from branding/Logo.cpp by tools/rendericons.
# ---------------------------------------------------------------------------
install(TARGETS misuc_app RUNTIME DESTINATION bin)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../assets/desktop/misuc.desktop
        DESTINATION share/applications)

foreach(SIZE 16 22 24 32 48 64 128 256 512)
  install(FILES
      ${CMAKE_CURRENT_SOURCE_DIR}/../assets/hicolor/${SIZE}x${SIZE}/apps/misuc.png
      DESTINATION share/icons/hicolor/${SIZE}x${SIZE}/apps)
endforeach()

# ---------------------------------------------------------------------------
# Debian packaging with CPack (runs `cmake --build build --target package`).
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "misuc")
set(CPACK_PACKAGE_VERSION "0.1.0")
set(CPACK_PACKAGE_RELEASE "1")
set(CPACK_PACKAGE_VENDOR "Misuc")
set(CPACK_PACKAGE_CONTACT "misuc@localhost")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Multi-track MIDI sequencer and audio editor for Linux")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_NAME}_${CPACK_PACKAGE_VERSION}-${CPACK_PACKAGE_RELEASE}_amd64.deb")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://example.invalid/misuc")
include(CPack)