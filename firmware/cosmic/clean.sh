#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo "Cleaning build directory: $BUILD_DIR"

if [[ -d "$BUILD_DIR" ]]; then
    rm -rf "$BUILD_DIR"
    echo "Removed $BUILD_DIR"
else
    echo "Build directory does not exist"
fi

echo "Clean complete"
