cmake_minimum_required(VERSION 3.21)
project(query_runner LANGUAGES CXX)

add_library(query_runner INTERFACE)
target_include_directories(query_runner INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_compile_features(query_runner INTERFACE cxx_std_26)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(query_runner INTERFACE -freflection)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(QR_PQXX REQUIRED IMPORTED_TARGET libpqxx)
target_link_libraries(query_runner INTERFACE PkgConfig::QR_PQXX)
