#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-linux}"
APP="${APP:-${BUILD_DIR}/aircraft_qt}"

if [[ ! -x "${APP}" ]]; then
  echo "Executable not found: ${APP}" >&2
  echo "Run scripts/build_linux.sh first." >&2
  exit 1
fi

cd "${ROOT_DIR}"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
export AIRCRAFT_PREPROCESS_PYTHON="${AIRCRAFT_PREPROCESS_PYTHON:-${PYTHON:-python3}}"
export AIRCRAFT_TRAINING_PYTHON="${AIRCRAFT_TRAINING_PYTHON:-${PYTHON:-python3}}"
if [[ -n "${QT_ROOT:-}" && -d "${QT_ROOT}/lib" ]]; then
  export LD_LIBRARY_PATH="${BUILD_DIR}:${QT_ROOT}/lib:${LD_LIBRARY_PATH:-}"
else
  export LD_LIBRARY_PATH="${BUILD_DIR}:${LD_LIBRARY_PATH:-}"
fi

exec "${APP}" "$@"
