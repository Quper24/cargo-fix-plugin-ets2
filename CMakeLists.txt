cmake_minimum_required(VERSION 3.25)
project(CargoControlETS2 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build this plugin with 64-bit MSVC on Windows.")
endif()

set(SCS_SDK_ROOT "" CACHE PATH "Path to the extracted SCS Telemetry SDK")
if(NOT EXISTS "${SCS_SDK_ROOT}/include/scssdk_telemetry.h")
  message(FATAL_ERROR "Set SCS_SDK_ROOT to the extracted SCS Telemetry SDK folder.")
endif()

configure_file(src/branding.rc.in "${CMAKE_CURRENT_BINARY_DIR}/branding.rc" @ONLY)
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/branding.rc" PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/quper-logo.png")
add_library(ets2_cargo_market_control SHARED
  src/plugin.cpp
  src/notifications.cpp
  src/freight.cpp
  src/branding.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/branding.rc"
  src/version.rc
  src/CargoControlETS2.def
)
target_compile_features(ets2_cargo_market_control PRIVATE cxx_std_17)
target_include_directories(ets2_cargo_market_control PRIVATE "${SCS_SDK_ROOT}/include")
target_compile_definitions(ets2_cargo_market_control PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ets2_cargo_market_control PRIVATE bcrypt user32 gdi32 shell32 ole32 gdiplus)
target_compile_options(ets2_cargo_market_control PRIVATE /utf-8)
set_target_properties(ets2_cargo_market_control PROPERTIES
  OUTPUT_NAME "CargoControlETS2"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

option(CARGO_CONTROL_BUILD_CHECKS "Build offline generation-limit checks" OFF)
if(CARGO_CONTROL_BUILD_CHECKS)
  enable_testing()
  add_executable(generation_limit_check tests/generation_limit_test.cpp)
  target_include_directories(generation_limit_check PRIVATE src)
  target_compile_features(generation_limit_check PRIVATE cxx_std_17)
  add_test(NAME generation_limit COMMAND generation_limit_check)
endif()

option(CARGO_CONTROL_BUILD_PREVIEW "Build the offscreen notification preview" OFF)
if(CARGO_CONTROL_BUILD_PREVIEW)
  add_executable(notification_preview tools/notification_preview.cpp src/branding.cpp "${CMAKE_CURRENT_BINARY_DIR}/branding.rc")
  target_include_directories(notification_preview PRIVATE src)
  target_compile_features(notification_preview PRIVATE cxx_std_17)
  target_compile_definitions(notification_preview PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(notification_preview PRIVATE /utf-8)
  target_link_libraries(notification_preview PRIVATE user32 gdi32 gdiplus ole32)
endif()


