#!/bin/zsh
set -eu
cd "$(dirname "$0")"
mkdir -p build
xcrun clang -fobjc-arc -O2 -Wall -Wno-unused-parameter -mmacosx-version-min=15.0 lgmirror.m -o build/lgmirror \
  -framework Foundation -framework CoreGraphics -framework IOKit -framework ScreenCaptureKit \
  -framework CoreMedia -framework CoreVideo
codesign -s - --force build/lgmirror
file build/lgmirror
