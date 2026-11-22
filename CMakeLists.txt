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
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(OPENH264 REQUIRED IMPORTED_TARGET openh264)
  add_library(eme_openh264 src/processing/encoder/openh264_encoder.cpp)
  add_library(eme::openh264 ALIAS eme_openh264)
  target_link_libraries(eme_openh264 PUBLIC eme::core PRIVATE PkgConfig::OPENH264)
  target_compile_options(eme_openh264 PRIVATE ${EME_WARNINGS})
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
