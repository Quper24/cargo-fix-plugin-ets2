cmake_minimum_required(VERSION 3.25)
project(CargoControlETS2 LANGUAGES C CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build this plugin with 64-bit MSVC on Windows.")
endif()

set(SCS_SDK_ROOT "" CACHE PATH "Path to the extracted SCS Telemetry SDK")
if(NOT EXISTS "${SCS_SDK_ROOT}/include/scssdk_telemetry.h")
  message(FATAL_ERROR "Set SCS_SDK_ROOT to the extracted SCS Telemetry SDK folder.")
endif()

add_library(qs_minhook STATIC vendor/minhook/src/buffer.c vendor/minhook/src/hook.c vendor/minhook/src/trampoline.c vendor/minhook/src/hde/hde64.c)
target_include_directories(qs_minhook PUBLIC vendor/minhook/include)
add_library(qs_imgui STATIC vendor/imgui/imgui.cpp vendor/imgui/imgui_draw.cpp vendor/imgui/imgui_tables.cpp vendor/imgui/imgui_widgets.cpp vendor/imgui/backends/imgui_impl_win32.cpp vendor/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(qs_imgui PUBLIC vendor/imgui)
target_link_libraries(qs_imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)

configure_file(src/branding.rc.in "${CMAKE_CURRENT_BINARY_DIR}/branding.rc" @ONLY)
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/branding.rc" PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/quper-logo.png")
add_library(ets2_cargo_market_control SHARED
  src/overlay_ui.cpp
  src/plugin.cpp
  src/game_profile.cpp
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
target_link_libraries(ets2_cargo_market_control PRIVATE qs_imgui qs_minhook dinput8 dxguid comctl32 bcrypt user32 gdi32 shell32 ole32 gdiplus)
target_compile_options(ets2_cargo_market_control PRIVATE /utf-8)
set_target_properties(ets2_cargo_market_control PROPERTIES
  OUTPUT_NAME "CargoControlETS2"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

option(CARGO_CONTROL_BUILD_CHECKS "Build offline generation-limit checks" OFF)
if(CARGO_CONTROL_BUILD_CHECKS)
  enable_testing()
  add_executable(game_profile_check tests/game_profile_test.cpp src/game_profile.cpp)
  target_include_directories(game_profile_check PRIVATE src)
  target_compile_features(game_profile_check PRIVATE cxx_std_17)
  target_compile_definitions(game_profile_check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(game_profile_check PRIVATE bcrypt)
  add_test(NAME game_profile_selection COMMAND game_profile_check)
  foreach(snapshot temporary_1_61-1.61.1.1 oculus-1.61.1.1009 public_beta-1.62.0.10)
    set(exe_snapshot "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/executables/${snapshot}/eurotrucks2.exe")
    if(EXISTS "${exe_snapshot}")
      add_test(NAME "profile_${snapshot}" COMMAND game_profile_check "${exe_snapshot}")
    endif()
  endforeach()
  set(reference_image "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/executables/reference-1.61.1.9-mapped.bin")
  if(EXISTS "${reference_image}")
    add_test(NAME profile_reference_1.61.1.9 COMMAND game_profile_check "${reference_image}" --mapped-reference)
  endif()
  add_executable(generation_limit_check tests/generation_limit_test.cpp)
  target_include_directories(generation_limit_check PRIVATE src)
  target_compile_features(generation_limit_check PRIVATE cxx_std_17)
  add_test(NAME generation_limit COMMAND generation_limit_check)
  add_executable(hotkey_binding_check tests/hotkey_binding_test.cpp)
  target_include_directories(hotkey_binding_check PRIVATE src)
  target_compile_features(hotkey_binding_check PRIVATE cxx_std_17)
  add_test(NAME hotkey_binding COMMAND hotkey_binding_check)
  add_executable(menu_pointer_check tests/menu_pointer_test.cpp)
  target_include_directories(menu_pointer_check PRIVATE src)
  target_compile_features(menu_pointer_check PRIVATE cxx_std_17)
  target_compile_definitions(menu_pointer_check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  add_test(NAME menu_pointer COMMAND menu_pointer_check)
  add_executable(notification_lifetime_check tests/notification_lifetime_test.cpp)
  target_include_directories(notification_lifetime_check PRIVATE src)
  target_compile_features(notification_lifetime_check PRIVATE cxx_std_17)
  target_compile_definitions(notification_lifetime_check PRIVATE NOMINMAX)
  add_test(NAME notification_lifetime COMMAND notification_lifetime_check)
  add_executable(overlay_dx11_check tests/overlay_dx11_test.cpp src/overlay_ui.cpp src/branding.cpp "${CMAKE_CURRENT_BINARY_DIR}/branding.rc")
  target_include_directories(overlay_dx11_check PRIVATE src)
  target_compile_features(overlay_dx11_check PRIVATE cxx_std_17)
  target_compile_definitions(overlay_dx11_check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(overlay_dx11_check PRIVATE /utf-8)
  target_link_libraries(overlay_dx11_check PRIVATE qs_imgui qs_minhook dinput8 dxguid user32 gdi32 gdiplus ole32)
  add_test(NAME overlay_dx11 COMMAND overlay_dx11_check)
  add_executable(company_list_check tests/company_list_test.cpp)
  target_include_directories(company_list_check PRIVATE src)
  target_compile_features(company_list_check PRIVATE cxx_std_17)
  target_compile_definitions(company_list_check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  add_test(NAME company_list COMMAND company_list_check)
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


