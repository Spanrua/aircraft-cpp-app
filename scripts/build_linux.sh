#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-linux}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

cmake_args=(
  -S "${ROOT_DIR}"
  -B "${BUILD_DIR}"
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
  -DAIRCRAFT_ENABLE_UHD="${AIRCRAFT_ENABLE_UHD:-OFF}"
  -DAIRCRAFT_ENABLE_NI_VISA="${AIRCRAFT_ENABLE_NI_VISA:-OFF}"
  -DBUILD_TESTING="${BUILD_TESTING:-OFF}"
)

if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
  cmake_args+=("-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}")
fi

cmake "${cmake_args[@]}"
cmake --build "${BUILD_DIR}" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"

echo
echo "Build complete:"
echo "  ${BUILD_DIR}/aircraft_qt"
