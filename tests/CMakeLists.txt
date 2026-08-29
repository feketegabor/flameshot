find_package(Qt${QT_VERSION_MAJOR} CONFIG REQUIRED Core Gui Test)

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
