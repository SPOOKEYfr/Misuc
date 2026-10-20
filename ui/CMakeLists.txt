find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

# ---------------------------------------------------------------------------
# Canonical version: project(misuc VERSION ...) in the top-level CMakeLists.txt
# is the single source of truth. Every consumer (the app binary, the About
# dialog / splash / window title, Qt application metadata, and the CPack
# .deb / tarball package names) derives from this generated header and from
# ${PROJECT_VERSION}. Bump the version in the top-level CMakeLists.txt only.
# ---------------------------------------------------------------------------
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/branding/MisucVersion.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/branding/MisucVersion.h
  @ONLY)

set(MISUC_UI_SOURCES
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
  src/mixer/RoutingDialogs.cpp
  src/commands/MisucCommandRegistry.cpp
  src/tooltip/MisucTooltip.cpp
  src/playlist/Playlist.cpp
  src/playlist/PlaylistSelection.cpp
  src/RecordSetupDialog.cpp
  src/channel/ChannelRack.cpp
  src/browser/Browser.cpp
  src/inspector/Inspector.cpp
  src/project/DemoProject.cpp
  src/export/ExportDialog.cpp
  src/workspace/WorkspaceManager.cpp)

# Header-only QObject subclass: listed explicitly so AUTOMOC runs moc on it
# (findChild + vtable require the generated metaobject; MSVC static-asserts
# the Q_OBJECT contract at compile time, so the guard cannot be skipped).
set(MISUC_UI_MOC_HEADERS src/elide/ElideLabel.h)

# The UI sources compiled once, shared by the real app and the QA harness so
# GUI checks exercise the very same translation units shipped to users.
add_library(misuc_ui_objs OBJECT ${MISUC_UI_SOURCES} ${MISUC_UI_MOC_HEADERS})
target_include_directories(misuc_ui_objs PRIVATE src)
target_include_directories(misuc_ui_objs PUBLIC
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(misuc_ui_objs PRIVATE misuc_engine Qt6::Widgets)

add_executable(misuc_app src/main.cpp)
target_link_libraries(misuc_app PRIVATE
  misuc_ui_objs
  misuc_engine
  Qt6::Widgets)
if(WIN32)
  # HeaderBar CPU/RAM bookkeeping uses GetProcessMemoryInfo (psapi).
  target_link_libraries(misuc_app PRIVATE psapi)
endif()

# Qt-converted resources (window icon, about image, user guide text). Compile
# the .qrc explicitly with rcc - the AUTORCC path is unreliable here (Qt 6.4 /
# CMake 3.28) and silently embeds nothing, which gutted the user guide.
find_program(MISUC_RCC_EXECUTABLE NAMES rcc
  HINTS ${Qt6_DIR}/../../../bin /usr/lib/qt6/libexec ${Qt6_DIR}/../../../libexec)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/qrc_resources.cpp
  COMMAND ${MISUC_RCC_EXECUTABLE} --name resources
          --output ${CMAKE_CURRENT_BINARY_DIR}/qrc_resources.cpp
          ${CMAKE_CURRENT_SOURCE_DIR}/resources.qrc
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resources.qrc
          ${CMAKE_CURRENT_SOURCE_DIR}/resources/help/user-guide.txt
          ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/misuc-256.png
          ${CMAKE_CURRENT_SOURCE_DIR}/resources/about/obhup.png)
target_sources(misuc_app PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/qrc_resources.cpp)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/qrc_resources.cpp
  PROPERTIES GENERATED TRUE)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_definitions(misuc_app PRIVATE MISUC_DEBUG)
endif()

# ---------------------------------------------------------------------------
# QA harness: links the real UI objects plus a test driver (qa/qatool.cpp) and
# synthesizes real Qt mouse events against the live widgets. No test-only
# stubs in production code; everything it asserts is reachable GUI behaviour.
# ---------------------------------------------------------------------------
add_executable(misuc_qa ${CMAKE_CURRENT_SOURCE_DIR}/../qa/qatool.cpp)
target_include_directories(misuc_qa PRIVATE src)
target_link_libraries(misuc_qa PRIVATE
  misuc_ui_objs
  misuc_engine
  Qt6::Widgets)
if(WIN32)
  target_link_libraries(misuc_qa PRIVATE psapi)
endif()

# ---------------------------------------------------------------------------
# Linux desktop integration: install the app, a real .desktop entry, and the
# hicolor icon set rendered from branding/Logo.cpp by tools/rendericons.
# Windows ships a plain ZIP of windeployqt output instead (CI artifact), so
# the desktop-entry/icon-tree install only applies off-Windows.
# ---------------------------------------------------------------------------
if(NOT WIN32)
  install(TARGETS misuc_app RUNTIME DESTINATION bin)

  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../assets/desktop/misuc.desktop
          DESTINATION share/applications)

  foreach(SIZE 16 22 24 32 48 64 128 256 512)
    install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/../assets/hicolor/${SIZE}x${SIZE}/apps/misuc.png
        DESTINATION share/icons/hicolor/${SIZE}x${SIZE}/apps)
  endforeach()
endif()

# Windows side of the staged install (feeds the NSIS .exe installer + ZIP):
# the exe lands in the install root and windeployqt stages the Qt runtime
# NEXT to it inside the package tree at cpack time, so the installed app has
# no external Qt dependency. windeployqt must be on PATH when cpack runs
# (install-qt-action provides it on CI).
if(WIN32)
  install(TARGETS misuc_app RUNTIME DESTINATION .)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE DESTINATION .)
  install(CODE [[
    execute_process(COMMAND windeployqt --release --compiler-runtime
                            "${CMAKE_INSTALL_PREFIX}/misuc_app.exe"
                    RESULT_VARIABLE _wdqRc
                    OUTPUT_VARIABLE _wdqOut
                    ERROR_VARIABLE _wdqErr
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
    if(NOT _wdqRc EQUAL 0)
      message(WARNING "windeployqt failed (rc ${_wdqRc}): ${_wdqErr}")
    endif()
  ]])
endif()

# ---------------------------------------------------------------------------
# Release packaging with CPack (`cmake --build build --target package`):
# a Debian package (with automatic shared-library dependencies) and a plain
# tarball of the staged install layout for manual/system-agnostic installs.
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "misuc")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_RELEASE "1")
set(CPACK_PACKAGE_VENDOR "Misuc")
set(CPACK_PACKAGE_CONTACT "misuc@localhost")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Multi-track MIDI sequencer and audio workstation")
if(WIN32)
  set(CPACK_PACKAGE_FILE_NAME "misuc-${CPACK_PACKAGE_VERSION}-${CPACK_PACKAGE_RELEASE}-win64")
  # NSIS drives the double-click installer (.exe); the ZIP stays for portable
  # usage (u盘/stick installs where no registry writes are wanted).
  set(CPACK_GENERATOR "NSIS;ZIP")
  set(CPACK_NSIS_DISPLAY_NAME "Misuc ${CPACK_PACKAGE_VERSION}")
  set(CPACK_NSIS_PACKAGE_NAME "Misuc")
  set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
  set(CPACK_NSIS_MUI_FINISHPAGE_RUN "misuc_app.exe")
else()
  set(CPACK_PACKAGE_FILE_NAME "misuc-${CPACK_PACKAGE_VERSION}-${CPACK_PACKAGE_RELEASE}-linux")
  set(CPACK_GENERATOR "DEB;TGZ")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()
set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_NAME}_${CPACK_PACKAGE_VERSION}-${CPACK_PACKAGE_RELEASE}_amd64.deb")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://example.invalid/misuc")
set(CPACK_SOURCE_GENERATOR "TGZ")
include(CPack)