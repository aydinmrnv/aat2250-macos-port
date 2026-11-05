#!/bin/sh
# Stage 3 end to end, from Windows (Git Bash). Needs the owner's approval to run NVIDIA's nvcc in WSL (step 1).
#   1. WSL: nvcc builds the device halves (fatbins) of ggml-cuda and of tests/vecadd.cu
#   2. Mac Studio: Apple clang builds the host halves around those fatbins -> libggml-cuda.a; links llama-bench (CUDA)
#      and vecadd over libtinycudart + libtinycublas + nvrmcu; ad-hoc signs them
#   3. the binaries go into the kit on the OPENCORE stick (D:\cuda)
set -e
W=${W:-$(cd "$(dirname "$0")" && pwd)}
K="-i ${MAC_KEY:?set MAC_KEY to the ssh key for the Mac build host}"
M=${MAC_HOST:?set MAC_HOST=user@host, the Mac build host}
MSYS_NO_PATHCONV=1 wsl.exe -d apfsreader -u root -- bash $(echo $W | sed 's|^/c|/mnt/c|')/wsl/nvcc-all.sh
ls $W/fatbins/*.fatbin | wc -l
ssh $K $M 'rm -rf ~/macos-port-build/cuda-rm/fatbins && mkdir -p ~/macos-port-build/cuda-rm/fatbins'
scp -q $K $W/fatbins/*.fatbin $M:macos-port-build/cuda-rm/fatbins/
ssh $K $M 'cd ~/macos-port-build/cuda-rm && FATDIR=$PWD/fatbins zsh ggml-cuda-host.sh | grep -v "^ok" &&
  OUT=build/llama-cuda python3 build-llama.py --cuda build/libggml-cuda.a &&
  zsh host-cu.sh tests/vecadd.cu fatbins/vecadd.fatbin build/vecadd.o &&
  xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 build/vecadd.o build/libtinycublas.a build/libtinycudart.a build/libnvrmcu.a \
    -framework IOKit -framework CoreFoundation -lc++ -o build/vecadd &&
  codesign -f -s - build/vecadd build/llama-cuda/llama-bench-cuda && shasum -a 256 build/vecadd build/llama-cuda/llama-bench-cuda'
scp -q $K $M:macos-port-build/cuda-rm/build/vecadd $M:macos-port-build/cuda-rm/build/llama-cuda/llama-bench-cuda $W/kit/
cp $W/kit/vecadd $W/kit/llama-bench-cuda /d/cuda/ && sync && cmp $W/kit/vecadd /d/cuda/vecadd && cmp $W/kit/llama-bench-cuda /d/cuda/llama-bench-cuda
echo "stage 3 built and on the stick"
