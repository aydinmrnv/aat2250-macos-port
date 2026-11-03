#!/bin/zsh
# build.sh: build cuda-probe (x86_64 macOS). On Apple Silicon it runs under Rosetta.
set -eu
cd "$(dirname "$0")"
mkdir -p build
xcrun clang -arch x86_64 -mmacosx-version-min=13.0 -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter \
  -Wno-deprecated-declarations -o build/cuda-probe probe.c shim.c loader.c
file build/cuda-probe
