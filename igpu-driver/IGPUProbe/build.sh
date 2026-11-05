#!/bin/zsh
# build.sh : compile + link IGPUProbe.kext (x86_64) with the Xcode SDK. No project scripts, no network.
set -eu
cd "$(dirname "$0")"
SDK=$(xcrun --sdk macosx --show-sdk-path)
K=$SDK/System/Library/Frameworks/Kernel.framework/Headers
T=(-target x86_64-apple-macos26.0)
CXXF=($T -mkernel -fapple-kext -nostdinc -nostdinc++ -std=gnu++17 -O2 -Wall -Wextra -Werror
      -Wno-inconsistent-missing-override -Wno-deprecated-declarations -fno-rtti -fno-exceptions
      -DKERNEL -DAPPLE -DNeXT -I$K)

rm -rf build
mkdir -p build/IGPUProbe.kext/Contents/MacOS
xcrun clang++ $CXXF -c IGPUProbe.cpp -o build/IGPUProbe.o
xcrun clang $T -mkernel -nostdinc -DKERNEL -I$K -Wall -Werror -c kmod_info.c -o build/kmod_info.o
xcrun clang++ $T -mkernel -nostdlib -Xlinker -kext -L$SDK/usr/lib \
  build/IGPUProbe.o build/kmod_info.o -lkmodc++ -lkmod -lcc_kext \
  -o build/IGPUProbe.kext/Contents/MacOS/IGPUProbe
cp Info.plist build/IGPUProbe.kext/Contents/Info.plist
plutil -lint build/IGPUProbe.kext/Contents/Info.plist
codesign -s - --force build/IGPUProbe.kext
file build/IGPUProbe.kext/Contents/MacOS/IGPUProbe
