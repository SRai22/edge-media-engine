cmake_minimum_required(VERSION 3.25)
project(edge_media_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(EME_BUILD_TESTS "Build unit and integration tests" ON)
option(EME_BUILD_APPS "Build example applications" ON)

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
  target_compile_options(eme_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(eme_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

if(EME_BUILD_APPS)
  add_executable(synthetic_streamer apps/synthetic_streamer/main.cpp)
  target_link_libraries(synthetic_streamer PRIVATE eme::core)
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
  include(GoogleTest)
  gtest_discover_tests(eme_tests DISCOVERY_MODE PRE_TEST)  # build must not run the binary (TSan needs setarch -R)
endif()
