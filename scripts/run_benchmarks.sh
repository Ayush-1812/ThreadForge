#!/usr/bin/env bash
set -euo pipefail

script_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "$script_root/.." && pwd)
build_dir=${THREADFORGE_BENCHMARK_BUILD_DIR:-"$project_root/build-benchmarks"}
output_dir="$project_root/benchmark-results"
if [[ $# -gt 0 && $1 != --* ]]; then
    output_dir=$1
    shift
fi
mkdir -p -- "$output_dir"
output_dir=$(cd -- "$output_dir" && pwd)

cmake -S "$project_root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release \
    -DTHREADFORGE_BUILD_TESTS=OFF -DTHREADFORGE_BUILD_EXAMPLES=OFF \
    -DTHREADFORGE_BUILD_BENCHMARKS=ON
cmake --build "$build_dir" --target threadforge_benchmarks \
    -j "${THREADFORGE_BUILD_JOBS:-1}"

{
    date -u
    uname -srmo
    "${CXX:-c++}" --version
    cmake --version
    if command -v lscpu >/dev/null 2>&1; then
        lscpu | sed -n '/^Architecture:/p; /^CPU(s):/p; /^Model name:/p; /^Thread(s) per core:/p; /^Core(s) per socket:/p'
    fi
} > "$output_dir/environment.txt"

"$build_dir/benchmarks/threadforge_benchmarks" \
    --benchmark_min_warmup_time=0.1 --benchmark_min_time=0.2s \
    --benchmark_repetitions=5 "$@" \
    --benchmark_out="$output_dir/results.json" --benchmark_out_format=json
python3 "$script_root/summarize_benchmarks.py" "$output_dir/results.json" \
    > "$output_dir/summary.md"
printf 'Reports written to %s\n' "$output_dir"
