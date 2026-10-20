find_package(Qt${QT_VERSION_MAJOR} CONFIG REQUIRED Core Gui Widgets Test)

add_executable(
  flameshot-smart-region-tests
  smartregiontest.cpp
  ${CMAKE_SOURCE_DIR}/src/widgets/capture/smartregiondetector.cpp)

target_include_directories(
  flameshot-smart-region-tests
  PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(
  flameshot-smart-region-tests
  PRIVATE project_options
          Qt${QT_VERSION_MAJOR}::Core
          Qt${QT_VERSION_MAJOR}::Gui
          Qt${QT_VERSION_MAJOR}::Test)

set_target_properties(flameshot-smart-region-tests PROPERTIES AUTOMOC ON)

if(APPLE)
  find_library(APPLICATION_SERVICES_FRAMEWORK ApplicationServices REQUIRED)
  find_library(CORE_GRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
  target_sources(
    flameshot-smart-region-tests
    PRIVATE ${CMAKE_SOURCE_DIR}/src/widgets/capture/macossmartregiondetector.cpp)
  target_link_libraries(
    flameshot-smart-region-tests
    PRIVATE ${APPLICATION_SERVICES_FRAMEWORK} ${CORE_GRAPHICS_FRAMEWORK})
elseif(WIN32)
  target_sources(
    flameshot-smart-region-tests
    PRIVATE ${CMAKE_SOURCE_DIR}/src/widgets/capture/windowssmartregiondetector.cpp)
  target_link_libraries(
    flameshot-smart-region-tests
    PRIVATE dwmapi ole32 uiautomationcore)
endif()

add_test(NAME smart-region COMMAND flameshot-smart-region-tests)

add_executable(
  flameshot-selection-widget-tests
  selectionwidgettest.cpp
  ${CMAKE_SOURCE_DIR}/src/widgets/capture/selectionwidget.cpp
  ${CMAKE_SOURCE_DIR}/src/utils/globalvalues.cpp)
target_include_directories(
  flameshot-selection-widget-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(
  flameshot-selection-widget-tests PRIVATE APP_VERSION="test" FLAMESHOT_GIT_HASH="test")
target_link_libraries(
  flameshot-selection-widget-tests
  PRIVATE project_options Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Test)
set_target_properties(flameshot-selection-widget-tests PROPERTIES AUTOMOC ON)
add_test(NAME selection-widget COMMAND flameshot-selection-widget-tests)
set_tests_properties(selection-widget PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
