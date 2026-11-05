#!/bin/bash
# Device halves, in the throwaway WSL distro (apfsreader): NVIDIA's nvcc 13.0.88 (from the verified redistributable
# archives in /opt/cuda-13.0) compiles every ggml-cuda translation unit, and the vecadd test, to sm_120a fatbins.
# Compile only: nothing here runs GPU code, and the distro has no GPU access in use.
set -u
export PATH=/opt/cuda-13.0/bin:$PATH
G=/opt/llama.cpp/ggml
OUT=/mnt/c/Users/User1/macos-port/cuda-rm-work/fatbins
mkdir -p $OUT
DEFS="-DGGML_CUDA_FORCE_MMQ -DGGML_CUDA_NO_VMM -DGGML_CUDA_USE_GRAPHS -DNDEBUG"
for k in F16 Q4_0 Q4_1 Q5_0 Q5_1 Q8_0 BF16; do for v in F16 Q4_0 Q4_1 Q5_0 Q5_1 Q8_0 BF16; do DEFS="$DEFS -DGGML_CUDA_FA_${k}_${v}=1"; done; done
ARGS="--fatbin -std=c++17 -O3 -gencode arch=compute_120a,code=sm_120a --extended-lambda --expt-relaxed-constexpr -I$G/include -I$G/src -I$G/src/ggml-cuda $DEFS"
nvcc --version | tail -1
nvcc --fatbin -std=c++17 -O3 -gencode arch=compute_120a,code=sm_120a -o $OUT/vecadd.fatbin /mnt/c/Users/User1/macos-port/cuda-rm-work/tests/vecadd.cu && echo "ok vecadd"
ls $G/src/ggml-cuda/*.cu $G/src/ggml-cuda/template-instances/*.cu > /tmp/tus.txt
start=$(date +%s)
xargs -P${JOBS:-20} -I{} sh -c 'b=$(basename {} .cu); if nvcc '"$ARGS"' -o '"$OUT"'/$b.fatbin {} > /tmp/nvcc-$b.log 2>&1; then echo "ok $b"; else echo "FAIL $b"; fi' < /tmp/tus.txt > /tmp/nvcc-results.txt
echo "nvcc: $(grep -c '^ok' /tmp/nvcc-results.txt) of $(wc -l < /tmp/tus.txt) in $(( $(date +%s) - start )) s"
for b in $(grep '^FAIL' /tmp/nvcc-results.txt | cut -d' ' -f2); do echo "--- $b"; grep -m3 error /tmp/nvcc-$b.log; done
