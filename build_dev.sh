#!/usr/bin/env bash
# Build the debug configuration from master_plan.md, section 1.2.
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$project_dir/build/debug"

# Preserve builds copied from another checkout or machine before configuring.
if [[ -f "$build_dir/CMakeCache.txt" ]] && \
   { ! grep -Fxq "CMAKE_HOME_DIRECTORY:INTERNAL=$project_dir/source" "$build_dir/CMakeCache.txt" || \
     ! grep -Fxq "CMAKE_CACHEFILE_DIR:INTERNAL=$build_dir" "$build_dir/CMakeCache.txt"; }; then
  backup_dir="$(mktemp -d "$project_dir/build/debug-backup.XXXXXX")"
  mv -- "$build_dir" "$backup_dir/debug"
  printf 'Previous build preserved in %s/debug\n' "$backup_dir"
fi

cmake -S "$project_dir/source" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCHECKED_BUILD=ON \
  -DENABLE_SHARED=OFF \
  -DENABLE_ASSEMBLY=OFF \
  -DCMAKE_C_FLAGS_DEBUG="-O0 -g3 -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS_DEBUG="-O0 -g3 -fno-omit-frame-pointer"

cmake --build "$build_dir" -j

"$build_dir/x265" --version
printf '\nBuild complete: %s/x265\n' "$build_dir"
