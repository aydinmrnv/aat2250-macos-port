#!/bin/zsh
# build.sh : compile + link IGPUScanout.kext (x86_64) with the Xcode SDK. No project scripts, no network.
set -eu
cd "$(dirname "$0")"
SDK=$(xcrun --sdk macosx --show-sdk-path)
K=$SDK/System/Library/Frameworks/Kernel.framework/Headers
T=(-target x86_64-apple-macos26.0)
CXXF=($T -mkernel -fapple-kext -nostdinc -nostdinc++ -std=gnu++17 -O2 -Wall -Wextra -Werror
      -Wno-inconsistent-missing-override -Wno-deprecated-declarations -Wno-unused-parameter -fno-rtti -fno-exceptions
      -DKERNEL -DAPPLE -DNeXT -I$K)

rm -rf build
mkdir -p build/IGPUScanout.kext/Contents/MacOS
xcrun clang++ $CXXF -c IGPUScanout.cpp -o build/IGPUScanout.o
xcrun clang $T -mkernel -nostdinc -DKERNEL -I$K -Wall -Werror -c kmod_info.c -o build/kmod_info.o
xcrun clang++ $T -mkernel -nostdlib -Xlinker -kext -L$SDK/usr/lib \
  build/IGPUScanout.o build/kmod_info.o -lkmodc++ -lkmod -lcc_kext \
  -o build/IGPUScanout.kext/Contents/MacOS/IGPUScanout
cp Info.plist build/IGPUScanout.kext/Contents/Info.plist
plutil -lint build/IGPUScanout.kext/Contents/Info.plist
codesign -s - --force build/IGPUScanout.kext
file build/IGPUScanout.kext/Contents/MacOS/IGPUScanout
