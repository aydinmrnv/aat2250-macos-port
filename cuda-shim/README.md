# cuda-shim: NVIDIA's Linux CUDA library inside a macOS process (CUDA step C3, part 1)

**What it does:** loads NVIDIA's **unmodified** Linux `libcuda.so.615.78.08` (x86_64 ELF) into an x86_64 macOS process.
- `loader.c` is an ELF loader. It maps the segments, applies dynamic relocations and runs the constructor.
- `shim.c` provides the Linux C library (glibc) functions the library imports, implemented on macOS.

**Rules:**
- No NVIDIA file is modified, disassembled or redistributed (NVIDIA driver LICENSE §2.2/§2.3; see `../deliverables/cuda/C0-REPORT.md`).
- Behaviour is observed only at the C-library interface (the call trace below), not by reading NVIDIA's machine code.

## Build and run (any Mac; Apple Silicon runs it under Rosetta)

```bash
zsh build.sh
```

```bash
arch -x86_64 build/cuda-probe /path/to/libcuda.so.615.78.08
```

**Environment variables:**
- `CUDASHIM_TRACE=1` logs the paths it touches, `dlopen`/`dlsym`, refused calls and every failing shim call.
- `CUDASHIM_TRACE=2` also logs every C-library call the library makes (ltrace-style).
- `CUDASHIM_LIBDIR` is where other `libnvidia-*.so` files are looked up.

`libcuda` comes from NVIDIA's Linux driver package 615.78.08, which you download yourself (see C0-REPORT for the URL and SHA-256).

## Results (2026-11-03, Mac Studio M1 Max, macOS 27, Rosetta)
| Gate | Result |
|---|---|
| **L1:** library loads, relocates, and its constructor returns | **PASS** (107 MB image; 71,437 RELATIVE + 248 symbol relocations) |
| **L2:** `cuGetErrorString(1)` → "invalid argument"; `cuDriverGetVersion` | **PASS**: NVIDIA's own message; version **13040** (CUDA 13.4 driver API) |
| **L3:** `cuInit(0)` behaves as on a Linux machine without the NVIDIA driver | **PASS**: returns **100 CUDA_ERROR_NO_DEVICE**, matching the Linux reference run (stable over 3 runs) |

## How L3 was matched to Linux (2026-11-03)
**The problem:** the first build returned **304 CUDA_ERROR_OPERATING_SYSTEM**, and no shim call failed along the way.

**The reference run:** the same libcuda, with `cuInit` under `strace`, in a throwaway `ubuntu:24.04` container on the Alienware (no GPU). The harness and trace are in `../evidence/cuda/`. On Linux it returns **100 CUDA_ERROR_NO_DEVICE**.

**The cause:** comparing the Linux syscalls with the shim's call log showed the divergence. Right after the process-shared lock setup, Linux creates an **eventfd**, starts a worker thread and makes **pipe2** pipes. During initialisation libcuda had looked up `pipe2`, `accept4`, `sched_getcpu` and `pthread_{get,set}affinity_np` with `dlsym`, and the shim returned NULL for them.

**The fix:** add those functions, plus an eventfd emulation and Linux `/proc` `readlink` semantics.

**The result:** cuInit now follows the Linux path step for step:
1. worker thread;
2. application profiles;
3. the MPS check;
4. `/sys/module/nvidia/initstate`, `/proc/driver/nvidia/params`, `/dev/nvidiactl` (its mknod is refused), `/proc/modules` and the PCI scan;
5. one RM ioctl;
6. **100 NO_DEVICE**.

**What this means:** the next missing piece is real: the device, the place where phase 2 starts.

## Known gaps (by design at this stage)
- **Device nodes and `ioctl`:** `/dev/nvidia*` and its ioctls are not implemented yet. That is phase 2, which goes to the NVRM IOKit user client.
- **UVM:** not implemented. It needs a kernel extension.
- **Signals:** `sigaction` reports success without installing a handler. There is no siginfo translation yet, and every such call is logged.
- **Process-shared objects:** pthread and semaphore objects marked process-shared are process-private on macOS (logged).
- **Refused calls:** `fork`/`exec`/`posix_spawn` (Linux helpers such as nvidia-modprobe), sockets (MPS: same outcome as Linux without an MPS daemon), `timerfd`, `mknod`, and raw syscalls other than `gettid`.
- **eventfd:** emulated with a socketpair (counter and semaphore semantics; writes are redirected to the peer).
- **CPU affinity:** `pthread_*affinity_np` accept requests and report all CPUs (macOS has no affinity); `sched_getcpu` returns 0.
- **Memory protection:** the image is mapped RWX as a whole. Per-segment protections are a TODO.
