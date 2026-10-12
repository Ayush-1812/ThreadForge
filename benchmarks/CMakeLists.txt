find_package(benchmark REQUIRED)

add_executable(threadforge_benchmarks scheduler_benchmarks.cpp)
target_link_libraries(threadforge_benchmarks PRIVATE threadforge::threadforge benchmark::benchmark)
target_compile_features(threadforge_benchmarks PRIVATE cxx_std_20)
threadforge_project_warnings(threadforge_benchmarks)
target_compile_definitions(threadforge_benchmarks PRIVATE
    THREADFORGE_BENCHMARK_VERSION="${PROJECT_VERSION}"
    THREADFORGE_BENCHMARK_BUILD_TYPE="$<CONFIG>")
