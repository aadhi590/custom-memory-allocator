#!/usr/bin/env bash
# Builds the project in Release mode and runs every benchmark executable,
# so the numbers in docs/benchmarks.md are reproducible by anyone cloning
# this repo. Linux x86-64 only, matching the rest of this project's scope.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build-release"

cmake -B "${build_dir}" -S "${repo_root}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${build_dir}" -j"$(nproc)"

echo
echo "======================================================================"
echo " Stage 1 (single global mutex)"
echo "======================================================================"
"${build_dir}/benchmark_allocator_stage1"

echo
echo "======================================================================"
echo " Stage 2 (per-size-class locks)"
echo "======================================================================"
"${build_dir}/benchmark_allocator_stage2"

echo
echo "======================================================================"
echo " Stage 3 (thread-local caching)"
echo "======================================================================"
"${build_dir}/benchmark_allocator_stage3"

echo
echo "======================================================================"
echo " Fragmentation-awareness indicator"
echo "======================================================================"
"${build_dir}/benchmark_fragmentation"
