#!/bin/zsh
# Host halves of every ggml-cuda translation unit, each embedding the fatbin nvcc built from it (FATDIR/<name>.fatbin),
# archived to build/libggml-cuda.a. With FATDIR unset this is a dry run against empty fatbins: it proves the host side
# compiles, which is everything except the nvcc step.
setopt sh_word_split
P=${P:-$HOME/macos-port-build/cuda-rm}
G=$P/llama.cpp/ggml
OBJ=$P/build/ggml-cuda-obj; LOGD=$P/build/ggml-cuda-logs
rm -rf $OBJ $LOGD; mkdir -p $OBJ $LOGD
# macuda's configuration (docs/03-cuda-shim-plan.md there): MMQ for quantised matmul, no VMM, graphs in, every FA pair
DEFS="-DGGML_CUDA_FORCE_MMQ -DGGML_CUDA_NO_VMM -DGGML_CUDA_USE_GRAPHS -DNDEBUG"
for k in F16 Q4_0 Q4_1 Q5_0 Q5_1 Q8_0 BF16; do for v in F16 Q4_0 Q4_1 Q5_0 Q5_1 Q8_0 BF16; do DEFS="$DEFS -DGGML_CUDA_FA_${k}_${v}=1"; done; done
INC="-I$G/include -I$G/src -I$G/src/ggml-cuda"
ls $G/src/ggml-cuda/*.cu $G/src/ggml-cuda/template-instances/*.cu > $LOGD/tus.txt
: > /tmp/empty.fatbin
one() {
  b=$(basename $1 .cu)
  fat=${FATDIR:+$FATDIR/$b.fatbin}; fat=${fat:-/tmp/empty.fatbin}
  if [ ! -s $fat ] && [ -n "$FATDIR" ]; then echo "FAIL $b (no fatbin)"; return; fi
  if zsh $P/host-cu.sh $1 $fat $OBJ/$b.o $INC $DEFS > $LOGD/$b.log 2>&1; then echo "ok $b"; else echo "FAIL $b"; fi
}
N=0
while read f; do one $f & N=$((N+1)); if [ $((N % ${JOBS:-10})) -eq 0 ]; then wait; fi; done < $LOGD/tus.txt
wait
total=$(wc -l < $LOGD/tus.txt | tr -d ' '); ok=$(ls $OBJ/*.o 2>/dev/null | wc -l | tr -d ' ')
echo "host-compiled $ok of $total ggml-cuda files"
for l in $LOGD/*.log; do grep -l "error" $l >/dev/null && { echo "--- $(basename $l .log)"; grep error $l | head -3; }; done | head -40
if [ "$ok" = "$total" ]; then rm -f $P/build/libggml-cuda.a; ar rcs $P/build/libggml-cuda.a $OBJ/*.o && echo "archived $P/build/libggml-cuda.a"; fi
