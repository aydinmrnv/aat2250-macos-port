#!/bin/sh
# Runs inside a throwaway ubuntu:24.04 container: /w is this folder (libcuda read-only), output goes to /w/out.
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends gcc libc6-dev strace >/dev/null
mkdir -p /tmp/lib /w/out
ln -sf /w/nv/libcuda.so.615.78.08 /tmp/lib/libcuda.so.1
gcc -O0 -o /tmp/cuinit /w/cuinit.c -ldl
echo "--- devices visible in the container:"; ls /dev | tr '\n' ' '; echo
LD_LIBRARY_PATH=/tmp/lib strace -f -tt -s 200 -o /w/out/strace.txt /tmp/cuinit 2>&1 | tee /w/out/stdout.txt
echo "--- strace lines: $(wc -l < /w/out/strace.txt)"
