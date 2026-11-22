cmake_minimum_required(VERSION 3.25)
project(edge_media_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(EME_BUILD_TESTS "Build unit and integration tests" ON)
option(EME_BUILD_APPS "Build example applications" ON)
option(EME_WITH_WEBRTC "Build the libdatachannel WebRTC transport (needs vcpkg deps)" ON)
option(EME_WITH_OPENH264 "Build the OpenH264 software encoder (needs vcpkg deps)" ON)

if(MSVC)
  add_compile_options(/utf-8)  # fmt/spdlog require it
endif()
if(WIN32)
  # Windows 10+; keep <windows.h> from defining min/max and pulling in winsock 1 (breaks std::min, httplib).
  add_compile_definitions(_WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

find_package(Threads REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(eme_core
  src/core/frame_pool.cpp
  src/core/graph.cpp
  src/processing/encoder_node.cpp
  src/sources/synthetic_video_source.cpp
  src/testing/mock_h264_encoder.cpp
  src/testing/counting_sink.cpp
)
add_library(eme::core ALIAS eme_core)
target_include_directories(eme_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(eme_core PUBLIC Threads::Threads spdlog::spdlog)
if(MSVC)
  set(EME_WARNINGS /W4 /permissive-)
else()
  set(EME_WARNINGS -Wall -Wextra -Wpedantic -Wconversion)
endif()
target_compile_options(eme_core PRIVATE ${EME_WARNINGS})

if(EME_WITH_OPENH264)
  if(NOT WIN32)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPENH264 REQUIRED IMPORTED_TARGET openh264)
  else()  # vcpkg installs openh264 with a .pc file only; a pkg-config on PATH (e.g. Strawberry Perl's) won't see it
    find_path(OPENH264_INCLUDE_DIR wels/codec_api.h REQUIRED)
    find_library(OPENH264_LIBRARY NAMES openh264 REQUIRED)
    add_library(PkgConfig::OPENH264 UNKNOWN IMPORTED)
    set_target_properties(PkgConfig::OPENH264 PROPERTIES IMPORTED_LOCATION ${OPENH264_LIBRARY}
                          INTERFACE_INCLUDE_DIRECTORIES ${OPENH264_INCLUDE_DIR})
  endif()
  add_library(eme_openh264 src/processing/encoder/openh264_encoder.cpp)
  add_library(eme::openh264 ALIAS eme_openh264)
  target_link_libraries(eme_openh264 PUBLIC eme::core PRIVATE PkgConfig::OPENH264)
  target_compile_options(eme_openh264 PRIVATE ${EME_WARNINGS})
endif()

option(EME_WITH_CAMERA "Build the camera source (V4L2 on Linux, Media Foundation on Windows)" ON)
if(EME_WITH_CAMERA)
  find_package(libyuv CONFIG REQUIRED)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(eme_camera src/sources/v4l2/camera_v4l2.cpp)
  elseif(WIN32)
    find_package(wil CONFIG REQUIRED)
    add_library(eme_camera src/sources/windows_mf/camera_mf.cpp)
    target_link_libraries(eme_camera PRIVATE WIL::WIL mfplat mf mfreadwrite mfuuid ole32 delayimp)
    # Delay-loaded so executables still start where Media Foundation is not installed (see platform() in camera_mf.cpp).
    target_link_options(eme_camera INTERFACE /DELAYLOAD:mfplat.dll /DELAYLOAD:mf.dll /DELAYLOAD:mfreadwrite.dll)
  else()
    message(FATAL_ERROR "EME_WITH_CAMERA: no camera backend for ${CMAKE_SYSTEM_NAME} yet (Windows: EME-004)")
  endif()
  add_library(eme::camera ALIAS eme_camera)
  target_link_libraries(eme_camera PUBLIC eme::core PRIVATE yuv)
  target_compile_options(eme_camera PRIVATE ${EME_WARNINGS})
endif()

if(EME_WITH_WEBRTC)
  find_package(LibDataChannel CONFIG REQUIRED)
  find_package(nlohmann_json CONFIG REQUIRED)
  find_package(CURL CONFIG REQUIRED)
  add_library(eme_webrtc
    src/transport/webrtc_common/h264_track.cpp
    src/transport/webrtc_p2p/webrtc_peer_server.cpp
    src/transport/whip/http.cpp
    src/transport/whip/whip_publisher.cpp
  )
  add_library(eme::webrtc ALIAS eme_webrtc)
  target_link_libraries(eme_webrtc PUBLIC eme::core PRIVATE LibDataChannel::LibDataChannel nlohmann_json::nlohmann_json CURL::libcurl)
  target_compile_options(eme_webrtc PRIVATE ${EME_WARNINGS})
endif()

if(EME_BUILD_APPS)
  add_executable(synthetic_streamer apps/synthetic_streamer/main.cpp)
  target_link_libraries(synthetic_streamer PRIVATE eme::core)
  if(EME_WITH_CAMERA)
    find_package(nlohmann_json CONFIG REQUIRED)
    add_executable(camera_probe apps/camera_probe/main.cpp)
    target_link_libraries(camera_probe PRIVATE eme::camera nlohmann_json::nlohmann_json)
  endif()
  if(EME_WITH_CAMERA AND EME_WITH_WEBRTC AND EME_WITH_OPENH264)
    find_package(httplib CONFIG REQUIRED)
    # Embed the page so users run one binary and open one URL.
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/apps/camera_viewer/viewer.html EME_VIEWER_HTML)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/viewer_html.hpp.in
         "#pragma once\ninline constexpr const char kViewerHtml[] = R\"EMEHTML(@EME_VIEWER_HTML@)EMEHTML\";\n")
    configure_file(${CMAKE_CURRENT_BINARY_DIR}/generated/viewer_html.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/viewer_html.hpp @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS apps/camera_viewer/viewer.html)
    add_executable(camera_viewer apps/camera_viewer/main.cpp)
    target_include_directories(camera_viewer PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(camera_viewer PRIVATE eme::camera eme::webrtc eme::openh264 httplib::httplib nlohmann_json::nlohmann_json)
  endif()
  if(EME_WITH_WEBRTC AND EME_WITH_OPENH264)
    add_executable(webrtc_streamer apps/webrtc_streamer/main.cpp)
    target_link_libraries(webrtc_streamer PRIVATE eme::webrtc eme::openh264)
  endif()
endif()

# EME-014/015/016 spikes: SFU publisher comparison. Off by default; SDK path needs -DLiveKit_DIR=<sdk>/lib/cmake/LiveKit.
option(EME_BUILD_SPIKES "Build SFU publisher spike apps" OFF)
if(EME_BUILD_SPIKES AND EME_WITH_WEBRTC AND EME_WITH_OPENH264)
  add_executable(spike_whip_publisher apps/spikes/whip_publisher/main.cpp)
  target_link_libraries(spike_whip_publisher PRIVATE eme::webrtc eme::openh264 nlohmann_json::nlohmann_json)
  find_package(LiveKit CONFIG QUIET)
  if(LiveKit_FOUND)
    add_executable(spike_livekit_sdk_publisher apps/spikes/livekit_sdk_publisher/main.cpp)
    target_link_libraries(spike_livekit_sdk_publisher PRIVATE eme::openh264 LiveKit::livekit nlohmann_json::nlohmann_json)
    add_executable(spike_lk_subscriber apps/spikes/lk_subscriber/main.cpp)
    target_link_libraries(spike_lk_subscriber PRIVATE eme::core LiveKit::livekit nlohmann_json::nlohmann_json)
  else()
    message(STATUS "LiveKit SDK not found: SDK spike apps skipped")
  endif()
endif()

if(EME_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
  add_executable(eme_tests
    tests/unit/frame_pool_test.cpp
    tests/unit/distributor_test.cpp
    tests/unit/mock_encoder_test.cpp
    tests/unit/graph_test.cpp
    tests/integration/fanout_test.cpp
  )
  target_link_libraries(eme_tests PRIVATE eme::core GTest::gtest_main)
  target_include_directories(eme_tests PRIVATE tests)
  if(EME_WITH_CAMERA)
    target_sources(eme_tests PRIVATE tests/integration/camera_test.cpp)
    target_link_libraries(eme_tests PRIVATE eme::camera)
  endif()
  if(EME_WITH_OPENH264)
    target_sources(eme_tests PRIVATE tests/unit/openh264_encoder_test.cpp)
    target_link_libraries(eme_tests PRIVATE eme::openh264 PkgConfig::OPENH264)
  endif()
  if(EME_WITH_WEBRTC)
    target_sources(eme_tests PRIVATE tests/integration/webrtc_peer_test.cpp tests/unit/whip_http_test.cpp)
    target_include_directories(eme_tests PRIVATE src)
    target_link_libraries(eme_tests PRIVATE eme::webrtc LibDataChannel::LibDataChannel nlohmann_json::nlohmann_json CURL::libcurl)
  endif()
  include(GoogleTest)
  gtest_discover_tests(eme_tests DISCOVERY_MODE PRE_TEST)  # build must not run the binary (TSan needs setarch -R)
endif()
