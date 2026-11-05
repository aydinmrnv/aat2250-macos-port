#!/bin/zsh
# Host half of a split CUDA compile: Apple clang reads the .cu file, keeps only the CPU code, and embeds a fatbin that
# nvcc built from the same file (the device half) so the CUDA runtime layer can register its kernels at start-up.
#   host-cu.sh <src.cu> <fatbin> <out.o> [extra clang args...]
set -e
setopt sh_word_split
P=${P:-$HOME/macos-port-build/cuda-rm}
src=$1 fat=$2 out=$3; shift 3
CUDAP=$P/cuda-13
mkdir -p $CUDAP/bin
[ -f $CUDAP/version.txt ] || printf 'CUDA Version 12.8.0\n' > $CUDAP/version.txt
[ -f $CUDAP/version.json ] || printf '{"cuda":{"name":"CUDA SDK","version":"12.8.0"}}' > $CUDAP/version.json
# Apple clang 21's CUDA wrapper still includes the legacy texture-reference header that CUDA 13 removed; the API it
# declared is gone from CUDA 12+, so an empty stand-in is the faithful answer.
mkdir -p $P/compat && [ -f $P/compat/texture_fetch_functions.h ] || printf '#pragma once\n' > $P/compat/texture_fetch_functions.h
# The wrapper force-includes cuRAND's MTGP32 header only to neutralise its redeclaration of blockDim/threadIdx; with no
# cuRAND in the tree there is nothing to neutralise.
[ -f $P/compat/curand_mtgp32_kernel.h ] || printf '#pragma once\n' > $P/compat/curand_mtgp32_kernel.h
# _ALLOW_UNSUPPORTED_LIBCPP: NVIDIA's own opt-out of its "libc++ on x86" refusal. It concerns only this host half (macOS
# has no other C++ library); the device half is compiled by nvcc on Linux and is unaffected.
xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -std=c++17 ${HOSTOPT:--O2} -DNDEBUG -x cuda --cuda-host-only \
  --offload-arch=${ARCH:-sm_120a} --cuda-path=$CUDAP -nocudalib -Wno-unknown-cuda-version -D_ALLOW_UNSUPPORTED_LIBCPP \
  -include $P/vendor/macuda/cudart/tinycudart_compat.h -I$P/compat -I$CUDAP/include/cccl "$@" \
  -Xclang -fcuda-include-gpubinary -Xclang "$fat" -c "$src" -o "$out"
