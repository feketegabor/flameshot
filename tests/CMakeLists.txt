find_package(Qt${QT_VERSION_MAJOR} CONFIG REQUIRED Core Gui Widgets Test)

set(CAPTURE_DIR ${CMAKE_SOURCE_DIR}/src/widgets/capture)

function(flameshot_add_test name)
  cmake_parse_arguments(TEST "" "" "SOURCES;LIBRARIES" ${ARGN})
  add_executable(flameshot-${name}-tests ${TEST_SOURCES})
  target_include_directories(flameshot-${name}-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(
    flameshot-${name}-tests
    PRIVATE project_options Qt${QT_VERSION_MAJOR}::Test ${TEST_LIBRARIES})
  set_target_properties(flameshot-${name}-tests PROPERTIES AUTOMOC ON)
  add_test(NAME ${name} COMMAND flameshot-${name}-tests)
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

flameshot_add_test(
  smart-region
  SOURCES smartregiontest.cpp
          ${CAPTURE_DIR}/smartregioncontroller.cpp
          ${CAPTURE_DIR}/smartregiondetector.cpp
  LIBRARIES Qt${QT_VERSION_MAJOR}::Core)

flameshot_add_test(
  smart-region-geometry
  SOURCES smartregiongeometrytest.cpp
  LIBRARIES Qt${QT_VERSION_MAJOR}::Core)

flameshot_add_test(
  selection-widget
  SOURCES selectionwidgettest.cpp
          ${CAPTURE_DIR}/selectionwidget.cpp
          ${CMAKE_SOURCE_DIR}/src/utils/globalvalues.cpp
  LIBRARIES Qt${QT_VERSION_MAJOR}::Widgets)
target_compile_definitions(
  flameshot-selection-widget-tests PRIVATE APP_VERSION="test" FLAMESHOT_GIT_HASH="test")

if(WIN32)
  # Drives a real Notepad window, so it needs the Windows platform plugin.
  flameshot_add_test(
    windows-smart-region
    SOURCES windowssmartregiontest.cpp
            ${CAPTURE_DIR}/smartregiondetector.cpp
            ${CAPTURE_DIR}/windowssmartregiondetector.cpp
    LIBRARIES Qt${QT_VERSION_MAJOR}::Gui dwmapi ole32 uiautomationcore)
  target_compile_definitions(
    flameshot-windows-smart-region-tests PRIVATE FLAMESHOT_SMART_REGION_BACKEND)
  set_tests_properties(windows-smart-region PROPERTIES ENVIRONMENT "")
endif()
