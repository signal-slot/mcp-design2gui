#!/bin/bash
# Build script for mcp-design2gui (superbuild)
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Initialize submodules if needed
git submodule update --init --recursive

# Superbuild: builds qtpsd, qtmcp, then mcp-design2gui
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

echo "=== Build complete ==="
echo ""
echo "Binary: ./build/mcp-design2gui"
