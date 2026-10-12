cmake_minimum_required(VERSION 3.20)
project(ThreadForge VERSION 0.1.0 LANGUAGES CXX)

option(THREADFORGE_BUILD_TESTS "Build the GoogleTest test suite" ON)
option(THREADFORGE_BUILD_EXAMPLES "Build examples" ON)

find_package(Threads REQUIRED)

add_library(threadforge src/scheduler.cpp src/worker.cpp)
add_library(threadforge::threadforge ALIAS threadforge)
target_compile_features(threadforge PUBLIC cxx_std_20)
set_target_properties(threadforge PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(threadforge PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(threadforge PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(threadforge PRIVATE /W4 /permissive-)
else()
    target_compile_options(threadforge PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(THREADFORGE_BUILD_EXAMPLES)
    add_executable(threadforge_basic examples/basic.cpp)
    target_link_libraries(threadforge_basic PRIVATE threadforge::threadforge)
endif()

if(THREADFORGE_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    include(GoogleTest)
    add_executable(threadforge_tests tests/scheduler_test.cpp tests/queue_test.cpp)
    target_link_libraries(threadforge_tests PRIVATE threadforge::threadforge GTest::gtest_main)
    gtest_discover_tests(threadforge_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 30)
endif()
