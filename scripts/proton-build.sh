#!/usr/bin/env bash
# Build and install the Windows SDK with Zig for a Proton runtime.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${MODLOCK_PROTON_BUILD_DIR:-"$repo/build-proton"}
cd "$repo"
go mod download github.com/aperturerobotics/protobuf github.com/aperturerobotics/abseil-cpp
cmake -S "$repo" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$repo/cmake/zig-windows.cmake" -DBUILD_TESTING=OFF
# A workstation keeps half its cores free for its user. MODLOCK_BUILD_JOBS
# sets the count where the machine only builds, such as a CI runner.
cores=$(getconf _NPROCESSORS_ONLN)
cmake --build "$build_dir" --parallel "${MODLOCK_BUILD_JOBS:-$(( cores > 4 ? cores / 2 : 1 ))}"
cmake --install "$build_dir" --prefix "$build_dir/sdk"
echo "Windows SDK: $build_dir/sdk"
