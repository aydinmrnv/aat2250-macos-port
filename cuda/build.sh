#!/bin/zsh
# Build nvrmcu (the RM-backed driver), its self-test, and later the CUDA runtime layers, for x86_64 macOS.
# Runs on the Mac Studio (Apple Silicon) as a cross build: the Hackintosh is an Intel-architecture Mac.
set -e
setopt sh_word_split
P=${P:-$HOME/macos-port-build/cuda-rm}
OG=${OG:-$HOME/macos-port-build/ogkm615}
B=$P/build
mkdir -p $B
CC="xcrun clang -arch x86_64 -mmacosx-version-min=15.0"
NVINC="-I$OG/src/common/sdk/nvidia/inc -I$OG/kernel-open/common/inc -I$OG/src/nvidia/arch/nvalloc/unix/include -I$OG/src/common/inc"
CF="-std=gnu11 -O2 -g -Wall -Wno-unused-parameter -Wno-missing-braces -fPIC -I$P/vendor/macuda/include -I$P/vendor/macuda/src -I$P/src"
BID=$(cd $P && git rev-parse --short HEAD 2>/dev/null || echo nvrmcu-0.1)

for f in qmd cubin image err submit sha256; do $CC $CF -c $P/vendor/macuda/src/$f.c -o $B/$f.o; done
$CC $CF -DTINYNV_BUILD_ID="\"$BID\"" -c $P/vendor/macuda/src/build_id.c -o $B/build_id.o
(cd $P/vendor/macuda/cubins && xxd -i copy1d.cubin | sed 's/copy1d_cubin/nvrmcu_copy1d_cubin/g' > $B/copy1d_cubin.c)
$CC $CF -c $B/copy1d_cubin.c -o $B/copy1d_cubin.o
$CC $CF -c $P/src/nvrm_io.c -o $B/nvrm_io.o
$CC $CF $NVINC -c $P/src/nvrmcu.c -o $B/nvrmcu.o
rm -f $B/libnvrmcu.a
ar rcs $B/libnvrmcu.a $B/{qmd,cubin,image,err,submit,sha256,build_id,copy1d_cubin,nvrm_io,nvrmcu}.o
$CC $CF $P/src/selftest.c $B/libnvrmcu.a -framework IOKit -framework CoreFoundation -o $B/nvrmcu-selftest
file $B/nvrmcu-selftest
echo "built $B/libnvrmcu.a and $B/nvrmcu-selftest"

# --- stage 2: macuda's CUDA runtime (libtinycudart) and cuBLAS (libtinycublas), unchanged, on nvrmcu
CUDA=$P/cuda-13/include
CXX="xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0"
RT=$P/vendor/macuda/cudart
CRF="-std=gnu11 -O2 -g -Wno-unused-parameter -fPIC -I$P/vendor/macuda/include -I$RT"
for f in cudart fatbin hostpool; do $CC $CRF -c $RT/$f.c -o $B/rt_$f.o; done
for f in cudart_api graph; do $CC $CRF -I$CUDA -c $RT/$f.c -o $B/rt_$f.o; done
$CC $CRF -I$CUDA -c $P/src/cudart_extra.c -o $B/rt_extra.o
$CXX -std=c++17 -O2 -fPIC -I$P/vendor/llvm-demangle -c $RT/demangle.cpp -o $B/rt_demangle.o
$CXX -std=c++17 -O2 -fPIC -I$P/vendor/llvm-demangle -c $P/vendor/llvm-demangle/ItaniumDemangle.cpp -o $B/rt_itanium.o
for c in copy1d copy2d keepalive gemm; do
  (cd $P/vendor/macuda/cubins && xxd -i $c.cubin > $B/${c}_rtcubin.c)
  $CC $CRF -c $B/${c}_rtcubin.c -o $B/${c}_rtcubin.o
done
rm -f $B/libtinycudart.a $B/libtinycublas.a
ar rcs $B/libtinycudart.a $B/rt_{cudart,fatbin,hostpool,cudart_api,graph,demangle,itanium,extra}.o $B/{copy1d,copy2d,keepalive}_rtcubin.o
$CC $CRF -I$CUDA -c $P/vendor/macuda/cublas/cublas.c -o $B/rt_cublas.o
ar rcs $B/libtinycublas.a $B/rt_cublas.o $B/gemm_rtcubin.o
LIBS="$B/libtinycublas.a $B/libtinycudart.a $B/libnvrmcu.a -framework IOKit -framework CoreFoundation -lc++"
$CC -std=gnu11 -O2 -I$CUDA $P/tests/cuda-selftest.c $LIBS -o $B/cuda-selftest
file $B/cuda-selftest
echo "built $B/libtinycudart.a $B/libtinycublas.a $B/cuda-selftest"
