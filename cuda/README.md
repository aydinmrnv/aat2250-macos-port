# CUDA on a Hackintosh: nvrmcu

This folder is an attempt at real CUDA on the RTX 5090 under macOS on this PC (an x86 Hackintosh). As far as we could find (2026-11-05), nobody has done that before.

**Status (2026-11-05):**
- **Built:** stages 1 and 2 are built and committed. A ready-to-run test kit is in `/cuda` on the OPENCORE stick.
- **Untested on hardware:** nothing has run on the card yet. Every test needs macOS booted with the NVIDIA driver up.
- **Blocked:** stage 3 (llama.cpp) is blocked on running NVIDIA's compiler, which needs the owner's approval.

## Prior work
- **[macuda](https://github.com/agrogov/macuda)** (MIT, September 2026) runs unmodified llama.cpp and stable-diffusion.cpp with their CUDA backends on an RTX 5090.
  - Its setup is an Apple Silicon Mac with a Thunderbolt eGPU, through tinygrad's TinyGPU DriverKit extension.
  - Its driver boots the GPU itself: GSP firmware, page tables and rings. It needs the card to itself, so the card can't drive a display.
- **NullMoth's driver** is already installed here. It is NVIDIA's open kernel driver (open-gpu-kernel-modules **615.78.08**) ported to macOS, plus NVK (Mesa's Vulkan driver) and a Metal translation layer. It gives Metal, not CUDA.
- **Tiny Corp's TinyGPU** is the Apple-signed extension macuda builds on. Like macuda, it covers only an external GPU on Apple Silicon. It runs tinygrad, not CUDA.
- **ZLUDA** lists macOS as not supported.

## The idea
On this machine the NVIDIA resource manager (RM) is already running in the kernel and driving the desktop. So CUDA doesn't need to boot the card. It needs to be an ordinary RM client, the same kind NullMoth's Vulkan driver is:
- It asks the RM for memory, an address space, a channel and a compute object.
- It writes commands into its own ring.
- It rings the doorbell.

The display keeps running alongside.

```
your CUDA program (CUDA runtime API, cuBLAS API)
  └ macuda's libtinycudart + libtinycublas      (unchanged, MIT)
      └ nvrmcu  ← new: macuda's driver interface (tinynv.h) on NVIDIA RM calls
          └ IOKit → NullMoth's NVRM kext (NVIDIA open RM 615.78.08) → GSP firmware → RTX 5090
```

**Why this route** rather than loading NVIDIA's real Linux `libcuda.so` (see `deliverables/cuda/C0-REPORT.md` in the private tree):
- Running NVIDIA's closed library on another OS is a licence question we left open.
- That route needs a new kernel extension (UVM).

nvrmcu needs neither: it's open code all the way down to the kext, and it runs as a plain program with nothing installed.

## What each piece is
| Path | What | Origin |
|---|---|---|
| `src/nvrm_io.c` | IOKit transport to the NVRM kext: the escape, fd-bind and mapping calls | New. Protocol facts read from NullMoth's source; no code copied (NullMoth's licence is noncommercial) |
| `src/nvrmcu.c` | `tinynv.h` implemented on RM objects: device setup, address space, video and system memory, the channel (TSG, context share, GPFIFO), compute object, submission and completion, module upload, kernel launch | New. The RM object sequence follows NullMoth's Vulkan backend, which is proven on this kext |
| `src/cudart_extra.c` | A few runtime calls ordinary programs use that macuda leaves out | New |
| `vendor/macuda/` | QMD (launch descriptor) builder, cubin parser, image layout, command encoders; the CUDA runtime and cuBLAS layers; the sm_120 copy and GEMM kernels | macuda, unchanged, MIT (`vendor/macuda/LICENSE`, `NOTICE`) |
| `vendor/llvm-demangle/` | LLVM's Itanium demangler (macuda needs it, because Apple's can't parse some ggml kernel names) | LLVM 19.1.7, Apache 2.0 with LLVM exception |
| `src/selftest.c`, `tests/cuda-selftest.c` | The two tests in the kit | New |

**Design of version 0.1 (correctness first):**
- **One channel.** Every batch ends with a wait-for-idle semaphore release, so work runs strictly in order.
- **All copies by a kernel.** Host↔device copies, memset and device-to-device copies are done by macuda's small copy kernel on the compute engine, through a 64 MB pinned system-memory staging ring. No assumption is made about how much video memory the CPU can reach through BAR1.
- **The copy kernel's own image** travels in the command stream as inline data.
- **Not yet:** CUDA graphs, event timing and pinned host memory. Each one is refused by name, and the layers above fall back.

## Build (on the Mac Studio, cross-compiling for x86_64)
```bash
P=~/macos-port-build/cuda-rm OG=~/macos-port-build/ogkm615 zsh build.sh
```

**Needs:**
- Xcode;
- NVIDIA's open-gpu-kernel-modules checked out at tag `615.78.08` (`OG`);
- the CUDA 13.0 headers in `$P/cuda-13/include`.

The headers come from NVIDIA's redistributable archives (`redistrib_13.0.3.json`; each archive is checked against NVIDIA's SHA-256). They're not committed: the CUDA EULA doesn't allow it.

## Test
Boot macOS, then:
```bash
sh /Volumes/OPENCORE/cuda/run
```

The results go to the stick. If anything fails, the run repeats it with every RM call traced (`NVRMCU_TRACE=1`).

## Stage 3: llama.cpp (blocked)
ggml's CUDA backend has to be compiled for sm_120a with NVIDIA's `nvcc`, which only runs on Linux.
- **Already done:** NVIDIA's official CUDA 13.0.3 compiler is downloaded and verified in a throwaway WSL distro (`apfsreader`, `/opt/cuda-13.0`).
- **Blocked:** running it is a downloaded program, which needs the owner's approval.
- **After approval:** device code is built with nvcc in WSL, and host code with Apple clang (`-x cuda --cuda-host-only`, tested and working). Then ggml-cuda is linked against this stack, as macuda does. llama.cpp is checked out at macuda's pinned commit `ad6c668` plus `2f53959`.
