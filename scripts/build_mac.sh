#!/bin/bash
# Build ClaudeTune on your Mac and install it for the current user.
# Needs: Xcode Command Line Tools (xcode-select --install) and CMake (brew install cmake).
set -euo pipefail
cd "$(dirname "$0")/.."

if ! xcode-select -p >/dev/null 2>&1; then
  echo "Installing the Xcode Command Line Tools first - re-run this script when that finishes."
  xcode-select --install
  exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "CMake is missing. Install it with:  brew install cmake   (or from https://cmake.org/download/)"
  exit 1
fi

ARCH="$(uname -m)"
echo "==> Configuring for $ARCH"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$ARCH"

echo "==> Building (the first build downloads JUCE and takes a few minutes)"
cmake --build build --config Release -j "$(sysctl -n hw.ncpu)" \
  --target ClaudeTune_VST3 ClaudeTune_AU ClaudeTune_Standalone ClaudeTuneEngineTests

echo "==> Running engine tests"
./build/ClaudeTuneEngineTests

ART=build/ClaudeTune_artefacts/Release
VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"
mkdir -p "$VST3_DIR" "$AU_DIR"

echo "==> Installing"
rm -rf "$VST3_DIR/ClaudeTune.vst3" "$AU_DIR/ClaudeTune.component"
cp -R "$ART/VST3/ClaudeTune.vst3" "$VST3_DIR/"
cp -R "$ART/AU/ClaudeTune.component" "$AU_DIR/"
codesign --force --deep --sign - "$VST3_DIR/ClaudeTune.vst3"
codesign --force --deep --sign - "$AU_DIR/ClaudeTune.component"
killall -9 AudioComponentRegistrar 2>/dev/null || true

echo
echo "Done. ClaudeTune is installed in:"
echo "  $VST3_DIR/ClaudeTune.vst3"
echo "  $AU_DIR/ClaudeTune.component"
echo "Standalone app: $ART/Standalone/ClaudeTune.app"
echo "Restart your DAW (or rescan plug-ins) to see it."
