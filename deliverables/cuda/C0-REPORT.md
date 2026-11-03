# C0: can NVIDIA's Linux CUDA library run on this macOS stack? (2026-11-03)

**Goal:** find out how big the "real CUDA on macOS" path is before anyone builds it. The path is: a loader for Linux ELF libraries plus a glibc shim, a device shim onto the macOS NVRM driver, and a minimal UVM kext.

Nothing in this step ran NVIDIA code or touched the GPU.

## Inputs
- **Package:** `NVIDIA-Linux-x86_64-615.78.08.run` from download.nvidia.com, 529,834,675 B.
  - SHA-256 `3e3f6ddd…7ffe`, which **matches NVIDIA's published `.sha256sum`**.
  - The payload was extracted with Python's zstd and tarfile modules. The installer was never run.
  - Location: `~/macos-port-build/cuda-c0/x` on the Mac Studio (not in the repo; NVIDIA's files are not redistributed).
- **Version match:** `libcuda.so.615.78.08` (sha256 `f7332093…dfc3`) is the same RM version as the nullmoth NVRM kext (ogkm 615.78.08).
- **GPU support:** `supported-gpus.json` lists `0x2B85` "NVIDIA GeForce RTX 5090" with the `kernelopen` feature.

## Licence finding (read this first)
`LICENSE` §2.2: *"You may not reverse engineer, decompile, or disassemble the SOFTWARE provided in binary form…"*. §2.3 forbids modifying the binaries.

- **What went wrong:** during this step I disassembled `libcuda.so` (llvm-objdump `-d`) to collect syscall and ioctl constants. That is against §2.2. The disassembly and the scan script were **deleted** once I noticed.
- **What this report uses instead:**
  - ELF metadata that any loader has to read (program headers, the dynamic section and the dynamic symbol table, which together give the import table);
  - path strings;
  - NVIDIA's **open-source** kernel code, which defines the same interfaces.
- **The rule for all later steps:** no disassembly or decompilation of NVIDIA binaries.
  - Loading the unmodified library and observing it at its interface (which device commands it issues) is how any OS runs it. Whether doing that on a non-Linux OS fits "install and use" (§1.1a) is a **legal question for the user to judge**. This report is not legal advice.
  - The library must stay byte-for-byte unmodified (§2.3).

## Findings (from metadata and open source)
| Question | Answer | Effect on effort |
|---|---|---|
| Libraries it links | `libc`, `libpthread`, `libdl`, `librt`, `libm` only | A glibc shim is enough. No C++ runtime or other dependencies. |
| Imports | 248, all glibc (226 at GLIBC_2.2.5) | A bounded shim |
| ELF thread-local storage | **None in libcuda** (no PT_TLS). `libnvidia-ptxjitcompiler` has none either. `libnvidia-nvvm` and `libnvidia-gpucomp` do. | Big simplification. NVVM/gpucomp are only needed for NVVM-IR/LTO paths, so ship cubins built for sm_120 and skip them at first. |
| Relocation types | RELATIVE, GLOB_DAT, JUMP_SLOT, one R_X86_64_64 | A simple loader |
| CPU architecture | x86_64 Linux on an x86_64 Mac (the Alienware) | No instruction translation. The SysV calling convention is the same on macOS. |
| Linux-only imports to emulate | `eventfd`, `timerfd_*`, `dlmopen`, `sysinfo`, `get_nprocs`, `__xstat`/`__fxstat` (stat layout), `mremap`, unnamed `sem_init`, `pthread_spin_*`, process-shared pthread attributes, `syscall()` | Medium. Every glibc struct (pthread types, `stat`, `dirent`, `sigaction`) and every errno value needs translating. |
| Linux files it reads | `/proc/driver/nvidia/params`, `/proc/self/{maps,exe,status,cmdline}`, `/proc/cpuinfo`, `/proc/devices`, `/sys/bus/pci/devices/…`, `/sys/devices/system/node…` | Synthesize them in the shim (a small virtual file table) |
| Device nodes | `/dev/nvidiactl`, `/dev/nvidia%d`, `/dev/nvidia-uvm`, `/dev/nvidia-uvm-tools`, `/dev/nvidia-caps/*` | See the next table |

## How the macOS NVRM driver compares (C1 preview, from its source)
- **How user space reaches it:** there are no `/dev` nodes. User space talks to it through IOKit (`IOServiceOpen("NVRM")`).
  - Selector 1 `sEsc(cmd, struct)` passes an escape number and parameter struct, and everything that isn't special-cased goes to RM's own `rm_ioctl`.
  - NVK's macOS port fakes file descriptors with `/dev/null` handles.
  - Memory maps go through `clientMemoryForType`; the Linux equivalent is mmap on the fd.
  - Events go through selector 6, `sWaitEvent`; the Linux equivalent is poll.

  So the device shim has a working template in nullmoth's own code.
- **RM escapes** (`NV_ESC_RM_ALLOC`, `CONTROL`, `FREE`, `MAP_MEMORY`, `MAP_MEMORY_DMA`, `DUP_OBJECT`, `SHARE`, `VID_HEAP_CONTROL`, `UPDATE_DEVICE_MAPPING_INFO`, …) are handled by `rm_ioctl`, so they are **already available**.
- **Linux-layer escapes that macOS lacks:** `ATTACH_GPUS_TO_FD`, `SYS_PARAMS`, `NUMA_INFO`, `QUERY_DEVICE_INTR`, `WAIT_OPEN_COMPLETE`, `IOCTL_XFER_CMD`, `EXPORT_TO_DMABUF_FD`.
  - Linux handles these in `nv.c`, which the macOS driver doesn't use.
  - They are small. Most can be answered in the user-space shim (the XFER unwrap, no-NUMA answers, open-complete) or with a few lines in the kext.
- **UVM is completely absent** (only stub functions). `/dev/nvidia-uvm` is required: CUDA does not initialise without it.
  - The open `nvidia-uvm` source (kernel-open, MIT/GPL dual-licensed) defines about 70 commands.
  - A first kext needs the non-managed-memory subset: `UVM_INITIALIZE`, `REGISTER_GPU`, `REGISTER_GPU_VASPACE`, `REGISTER_CHANNEL`, `CREATE_EXTERNAL_RANGE`, `MAP_EXTERNAL_ALLOCATION`, `FREE`, `ALLOC_SEMAPHORE_POOL`, `MAP_DYNAMIC_PARALLELISM_REGION`, `PAGEABLE_MEM_ACCESS`.
  - **This is the largest single piece**, and the exact minimum set is only known once CUDA's calls are traced at runtime, on the device-command interface, not by disassembly.

## Verdict
**No blocker was found at the static level.** The library is unusually shim-friendly: glibc only, no TLS, same CPU architecture, and an RM interface the macOS driver already exposes.

| Piece | Size | Gate |
|---|---|---|
| ELF loader + glibc shim (x86_64 macOS process) | Large, but well understood (FreeBSD's libc6-shim did the glibc half for this same library) | Loads libcuda; `cuGetErrorString` works (no GPU needed) |
| Device shim (`/dev/nvidia*` → IOKit NVRM) | Medium (NVK-macOS template) | `cuInit` gets past `/dev/nvidiactl` |
| Missing Linux escapes | Small | `cuDeviceGetName` → "NVIDIA GeForce RTX 5090" |
| Minimal UVM kext | Large | `cuCtxCreate`, `cuMemAlloc`, vector-add from an sm_120 cubin gives correct results |
| cudart / cuBLAS (CUDA toolkit ELF libraries) | Not analysed yet | cuBLAS SGEMM is bit-checked against the CPU |

**Every runtime gate needs the RTX 5090 working under macOS first** (T-BOOT / T-DISP in `a1-nvidia/macos-gpu-tests.md`). The loader and glibc shim can be developed and unit-tested beforehand on any x86_64 Mac or VM, without a GPU.
