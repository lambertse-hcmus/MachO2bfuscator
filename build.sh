#!/usr/bin/env bash
set -euo pipefail

# Usage: ./build.sh [debug|release] [--clean]
# Defaults to Release if not specified.
# Builds the CMake project in a dedicated ./build folder.

BUILD_TYPE="Release"
CLEAN=0

if [[ ${1:-} == "debug" ]]; then
  BUILD_TYPE="Debug"
elif [[ ${1:-} == "release" ]]; then
  BUILD_TYPE="Release"
elif [[ ${1:-} =~ ^(--help|-h)$ ]]; then
  echo "Usage: $0 [debug|release] [--clean]"
  echo "  debug     -> CMAKE_BUILD_TYPE=Debug"
  echo "  release   -> CMAKE_BUILD_TYPE=Release (default)"
  echo "  --clean   -> Remove build directory before configuring"
  exit 0
elif [[ ${1:-} != "" && ${1:-} != "--clean" ]]; then
  echo "[WARN] Unknown argument: $1 (expected 'debug' or 'release'). Using default: Release" >&2
fi

# Shift first arg if it was build type
if [[ ${1:-} == "debug" || ${1:-} == "release" ]]; then
  shift
fi

# Parse remaining args
for arg in "$@"; do
  case "$arg" in
    --clean)
      CLEAN=1
      ;;
    *)
      echo "[WARN] Ignoring unknown option: $arg" >&2
      ;;
  esac
done

BUILD_DIR="build"

if [[ $CLEAN -eq 1 ]]; then
  echo "[*] Cleaning $BUILD_DIR/"
  rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure
echo "[*] Configuring (CMAKE_BUILD_TYPE=${BUILD_TYPE})"
cmake -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" ..

# Build
echo "[*] Building"
cmake --build . --config "${BUILD_TYPE}"

# Optional: place build artifacts hint
echo "[✓] Build completed. Artifacts are in: $(pwd)"


