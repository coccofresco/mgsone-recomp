#!/bin/bash
# PS1Recomp -- Demo Build Script
# Builds the full project for demonstration/presentation

set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

echo "============================================"
echo "  PS1Recomp -- Demo Build"
echo "============================================"
echo "Project: $PROJECT_DIR"
echo "Build:   $BUILD_DIR"
echo ""

# Step 1: Configure
echo "[1/4] Configuring CMake..."
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5
echo ""

# Step 2: Build all targets
echo "[2/4] Building all targets..."
make -C "$BUILD_DIR" -j$(nproc) ps1Analyzer ps1Recomp ps1Runtime 2>&1 | tail -10
echo ""

# Step 3: Run tests
echo "[3/4] Running tests..."
echo "--- Analyzer Tests ---"
"$BUILD_DIR/ps1Test/ps1Analyzer_tests" --gtest_brief=1 2>&1 | tail -3
echo "--- Recompiler Tests ---"
"$BUILD_DIR/ps1Test/ps1Recomp_tests" --gtest_brief=1 2>&1 | tail -3
echo ""

# Step 4: Show binaries
echo "[4/4] Build artifacts:"
ls -lh "$BUILD_DIR/ps1Analyzer/ps1Analyzer" 2>/dev/null || echo "  ps1Analyzer: not found"
ls -lh "$BUILD_DIR/ps1Recomp/ps1Recomp" 2>/dev/null || echo "  ps1Recomp: not found"
ls -lh "$BUILD_DIR/ps1Runtime/ps1Runtime" 2>/dev/null || echo "  ps1Runtime: not found"
echo ""

echo "============================================"
echo "  Build Complete!"
echo "============================================"
echo ""
echo "Usage:"
echo "  # Analyze a PS1 disc:"
echo "  ./build/ps1Analyzer/ps1Analyzer game.bin output_config.toml"
echo ""
echo "  # Scan for overlays:"
echo "  ./build/ps1Analyzer/ps1Analyzer --scan-overlays game.bin overlays.toml"
echo ""
echo "  # Recompile:"
echo "  ./build/ps1Recomp/ps1Recomp output_config.toml"
echo ""
echo "  # Run (after recompiling runtime with generated code):"
echo "  ./build/ps1Runtime/ps1Runtime config.toml"
