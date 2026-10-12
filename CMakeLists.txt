cmake_minimum_required(VERSION 3.20)
project(ThreadForge VERSION 0.6.0 LANGUAGES CXX)

option(THREADFORGE_BUILD_TESTS "Build the GoogleTest test suite" ON)
option(THREADFORGE_BUILD_EXAMPLES "Build examples" ON)
option(THREADFORGE_BUILD_STRESS_TESTS "Build the heavier GoogleTest stress suite" ON)
option(THREADFORGE_BUILD_BENCHMARKS "Build the optional Google Benchmark suite" OFF)
option(THREADFORGE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(THREADFORGE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(THREADFORGE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(THREADFORGE_WARNINGS_AS_ERRORS "Treat project warnings as build errors" OFF)

# Keep our warning policy private: embedding ThreadForge must not change the
# consumer's warning flags. Apply it to tests/examples as well as the library.
function(threadforge_project_warnings target)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(THREADFORGE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(THREADFORGE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    elseif(THREADFORGE_WARNINGS_AS_ERRORS)
        message(FATAL_ERROR "Warnings-as-errors requires GCC, Clang, or MSVC")
    endif()
endfunction()

if(THREADFORGE_ENABLE_ASAN AND THREADFORGE_ENABLE_TSAN)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be combined")
endif()
if(THREADFORGE_ENABLE_ASAN OR THREADFORGE_ENABLE_UBSAN OR THREADFORGE_ENABLE_TSAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" OR MSVC)
        message(FATAL_ERROR "ThreadForge sanitizer builds require GCC or Clang")
    endif()
    set(threadforge_sanitizers "")
    if(THREADFORGE_ENABLE_ASAN)
        list(APPEND threadforge_sanitizers address)
    endif()
    if(THREADFORGE_ENABLE_UBSAN)
        list(APPEND threadforge_sanitizers undefined)
    endif()
    if(THREADFORGE_ENABLE_TSAN)
        list(APPEND threadforge_sanitizers thread)
    endif()
    list(JOIN threadforge_sanitizers "," threadforge_sanitizer_list)
    # Check both compilation and runtime linking, rather than accepting an
    # option that only fails when the real targets are linked later.
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=${threadforge_sanitizer_list} -fno-sanitize-recover=all")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=${threadforge_sanitizer_list}")
    string(REPLACE "," "_" threadforge_sanitizer_key "${threadforge_sanitizer_list}")
    set(threadforge_sanitizer_check "THREADFORGE_HAS_SANITIZER_${threadforge_sanitizer_key}")
    check_cxx_source_compiles("int main() { return 0; }" ${threadforge_sanitizer_check})
    cmake_pop_check_state()
    if(NOT ${threadforge_sanitizer_check})
        message(FATAL_ERROR "Compiler/runtime cannot compile and link ${threadforge_sanitizer_list} sanitizers")
    endif()
    add_library(threadforge_sanitizer_options INTERFACE)
    target_compile_options(threadforge_sanitizer_options INTERFACE
        -fsanitize=${threadforge_sanitizer_list} -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    target_link_options(threadforge_sanitizer_options INTERFACE
        -fsanitize=${threadforge_sanitizer_list})
endif()

find_package(Threads REQUIRED)

add_library(threadforge src/scheduler.cpp src/worker.cpp src/work_stealing_queue.cpp src/metrics.cpp)
add_library(threadforge::threadforge ALIAS threadforge)
target_compile_features(threadforge PUBLIC cxx_std_20)
set_target_properties(threadforge PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(threadforge PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(threadforge PUBLIC Threads::Threads)
if(TARGET threadforge_sanitizer_options)
    target_link_libraries(threadforge PUBLIC threadforge_sanitizer_options)
endif()
threadforge_project_warnings(threadforge)

if(THREADFORGE_BUILD_EXAMPLES)
    foreach(example IN ITEMS basic work_stealing priorities dependency_graph metrics_report)
        add_executable(threadforge_${example} examples/${example}.cpp)
        target_link_libraries(threadforge_${example} PRIVATE threadforge::threadforge)
        threadforge_project_warnings(threadforge_${example})
    endforeach()
endif()

if(THREADFORGE_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    include(GoogleTest)
    add_executable(threadforge_tests tests/scheduler_test.cpp tests/queue_test.cpp
        tests/stealing_test.cpp tests/priority_test.cpp tests/priority_api_test.cpp
        tests/dependency_test.cpp tests/metrics_test.cpp tests/centralized_pool_test.cpp
        tests/hardening_test.cpp)
    target_link_libraries(threadforge_tests PRIVATE threadforge::threadforge GTest::gtest_main)
    threadforge_project_warnings(threadforge_tests)
    gtest_discover_tests(threadforge_tests DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 30)

    # Replacement allocation functions are confined to this executable so
    # fault injection cannot affect unrelated tests or benchmark runtimes.
    add_executable(threadforge_allocation_tests tests/allocation_failure_test.cpp)
    target_link_libraries(threadforge_allocation_tests PRIVATE threadforge::threadforge GTest::gtest_main)
    threadforge_project_warnings(threadforge_allocation_tests)
    gtest_discover_tests(threadforge_allocation_tests DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 60 LABELS allocation)

    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME BenchmarkSummary
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_summary.py)
        set_tests_properties(BenchmarkSummary PROPERTIES TIMEOUT 30 LABELS tools)
    endif()
    if(THREADFORGE_BUILD_STRESS_TESTS)
        add_executable(threadforge_stress_tests tests/stress_test.cpp tests/priority_stress_test.cpp
            tests/dependency_stress_test.cpp tests/hardening_stress_test.cpp)
        target_link_libraries(threadforge_stress_tests PRIVATE threadforge::threadforge GTest::gtest_main)
        threadforge_project_warnings(threadforge_stress_tests)
        gtest_discover_tests(threadforge_stress_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30
            PROPERTIES TIMEOUT 120 LABELS stress)
    endif()
endif()

if(THREADFORGE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
