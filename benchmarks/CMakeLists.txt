find_package(benchmark REQUIRED)

add_executable(threadforge_benchmarks scheduler_benchmarks.cpp)
target_link_libraries(threadforge_benchmarks PRIVATE threadforge::threadforge benchmark::benchmark)
target_compile_features(threadforge_benchmarks PRIVATE cxx_std_20)
set_target_properties(threadforge_benchmarks PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(threadforge_benchmarks PRIVATE
    THREADFORGE_BENCHMARK_VERSION="${PROJECT_VERSION}"
    THREADFORGE_BENCHMARK_BUILD_TYPE="$<CONFIG>")
if(MSVC)
    target_compile_options(threadforge_benchmarks PRIVATE /W4 /permissive-)
else()
    target_compile_options(threadforge_benchmarks PRIVATE -Wall -Wextra -Wpedantic)
endif()
