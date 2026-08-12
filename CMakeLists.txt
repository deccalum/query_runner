cmake_minimum_required(VERSION 3.21)
project(query_runner LANGUAGES CXX)

add_library(query_runner INTERFACE)
target_include_directories(query_runner INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_compile_features(query_runner INTERFACE cxx_std_23)

find_package(PkgConfig REQUIRED)
pkg_check_modules(QR_PQXX REQUIRED IMPORTED_TARGET libpqxx)
target_link_libraries(query_runner INTERFACE PkgConfig::QR_PQXX)

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(qr_tests tests/test_query_runner.cpp)
  target_link_libraries(qr_tests PRIVATE query_runner GTest::gtest_main)
  target_compile_options(qr_tests PRIVATE -Wall -Wextra)
  include(GoogleTest)
  gtest_discover_tests(qr_tests)
endif()
