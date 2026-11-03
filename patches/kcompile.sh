#!/bin/zsh
# kcompile.sh <rev> : compile NVRM.cpp and nvrm_fbinfo.cpp at <rev> as x86_64 kext objects (no link). Written by coordinator; no project scripts run.
set -u
cd ~/macos-port-build/nvidia-macos-driver
REV=$1; W=$(mktemp -d); git archive $REV kexts/NVRM | tar -x -C $W
SDK=$(xcrun --sdk macosx --show-sdk-path); K=$SDK/System/Library/Frameworks/Kernel.framework/Headers
O=~/macos-port-build/ogkm615
FLAGS=(-target x86_64-apple-macos15.0 -mkernel -fapple-kext -nostdinc -nostdinc++ -std=gnu++17 -O2 -Wall -Wno-unused-function
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DNV_DARWIN -DNV_UNIX -D_LANGUAGE_C -DNVRM -DPORT_IS_KERNEL_BUILD=1 -DPORT_IS_CHECKED_BUILD=0 -DPORT_MODULE_core=1 -DPORT_MODULE_atomic=1 -DPORT_MODULE_debug=1 -DPORT_MODULE_util=1 -DPORT_MODULE_memory=1 -DPORT_MODULE_string=1 -DPORT_MODULE_sync=1 -DPORT_MODULE_safe=1 -DPORT_MODULE_thread=0 -DPORT_MODULE_example=0 -DPORT_MODULE_mmio=0 -DPORT_MODULE_time=0 -DPORT_MODULE_crypto=0 -DPORT_MODULE_cpu=0 -DPORT_MODULE_quick_sort=0
  -I$W/kexts/NVRM -I$K -I$O/src/common/sdk/nvidia/inc -I$O/src/nvidia/arch/nvalloc/unix/include -I$O/src/common/inc -I$O/kernel-open/common/inc -I$O/src/nvidia/inc/libraries -I$O/src/nvidia/inc -Wno-inconsistent-missing-override)
for f in nvrm_fbinfo.cpp NVRM.cpp; do
  echo "== $REV $f"; xcrun clang++ $FLAGS -c $W/kexts/NVRM/$f -o $W/${f%.cpp}.o 2>&1 | grep -E "error" | sed "s|$W/||" | head -${2:-15}
  [ -f $W/${f%.cpp}.o ] && echo "OK $(file -b $W/${f%.cpp}.o)" || echo "FAILED"
done
rm -rf $W
