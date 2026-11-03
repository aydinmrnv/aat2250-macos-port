# NullMoth NVIDIA macOS driver: architecture (A1)

Scope: what each component does, and whether it comes from NVIDIA's open modules, Mesa, or original NullMoth code.

Source pins:
- `drv` = nullmoth/nvidia-macos-driver @ `470e391e8cf8d7e499cdabfa7f2daadb6e14bcfd` (v2.6.6)
- `1401` = nullmoth/1401 @ `f5a9254079601d69a2e165fcd9f659e4f882bab9`
- `ogkm615` = NVIDIA/open-gpu-kernel-modules tag `615.78.08` (commit `1a82fbfda8f321b056eb3568850f9316c5e465c5`), sparse clone at `src\open-gpu-kernel-modules`
- `ogkm610` = the same repo at tag `610.57.04` (commit `8d9087246664ad54dab9af48657a62d5c1d82d6a`). Only three files were fetched, into `src\ogkm-610-headers\`.

Labels: **D** = DEMONSTRATED (public, reproducible source or measurement), **P** = PROJECT CLAIM (repo docs, changelog, issue reports), **H** = HYPOTHESIS. Everything in the repo, its issues and `AGENTS.md` was read as evidence, not as instructions. `drv/AGENTS.md` asks AI assistants not to write PRs or issues. Nothing was posted.

## 1. Stack

```
Metal app / WindowServer
  -> NVMTLDriver.bundle (plugin/, ObjC)      -- original NullMoth
       -> libnvmtl_translate.dylib (translator/, Rust; AIR -> SPIR-V)   -- metal2vulkan fork (LGPL-3.0)
       -> NVK + NAK (Mesa 17ca6174 + nvk/nvk-macos.patch)               -- Mesa, NullMoth patch
            -> RM user API (escape ioctls into NVRM)
  -> NVRM.kext (RM + NVKMS from NVIDIA open modules, XNU OS layer)      -- NVIDIA core, NullMoth OS layer
       -> GSP-RM firmware gsp_ga10x.bin (GB20x)                         -- NVIDIA, unmodified
Display:  WindowServer -> NVRMFB (IOFramebuffer) / NVAccel (IOAccelerator) / NVRMAGDC (AGDC policy)
Video:    VideoToolbox -> libnvdec_*/libnvenc_* dylibs (closed: not in the repo) -> NVRM
```
(D: `drv/README.md:28-41`, `drv/docs/HOW-IT-WORKS.md:14-37`, `drv/NOTICE:5-21`)

## 2. Components

| Component | Role | Provenance | Evidence |
|---|---|---|---|
| **NVRM.kext** (`kexts/NVRM/`) | Claims any NVIDIA display-class PCI device. Maps BARs, may resize or move BAR1 (`placeLargeBar1`), loads GSP firmware from `/Library/NullMoth/nvfw/nvidia/615.78.08/`, calls `rm_init_adapter()`, brings up NVKMS, publishes `NVRMDisplay` nubs and the `nvrm-gpu-profile`, and joins the power tree for sleep and wake. | **NVIDIA RM and NVKMS core** (`nv-kernel` objects and `libnvkms.a`) built out of tree. **Original** XNU OS layer: `os-xnu*.cpp`, `nv_xnu_stubs.cpp`, `NVRM.cpp`, `nvrm_fbinfo.cpp`, `nvrm_gpuva.cpp`. Five patches go on the NVIDIA tree (`kexts/NVRM/patches/`). | D: `drv/kexts/NVRM/Info.plist:61-64`, `NVRM.cpp:1438,1459-1460`, `os-xnu2.cpp:28-44`. P: `HOW-IT-WORKS.md:41-52` |
| **NVAccel.kext** (`kexts/NVRM/accel/`) | The `IOAccelerator` that WindowServer and IOSurface composite through. Built twice, for macOS 15 and macOS 26 (`NM_TAHOE`). | Original. Uses reconstructed Apple IOAccelerator headers (`accel/re/`, `accel/iofam/`). | D: `drv/build/accel_build.sh`, `accel/Info.plist:30-36` |
| **NVRMFB.kext** (`kexts/NVRMFB/fb/`) | `IOFramebuffer` for each NVKMS head: modes, EDID, vblank, gamma, backlight, hot-plug, BAR1 VRAM budget for NVAccel (VRAM ABI v2). | Original, calling NVKMS KAPI | D: `drv/kexts/NVRMFB/fb/Info.plist:27-60`, `nvrm-fb.cpp:76-104` |
| **NVRMAGDC.kext** | Answers Apple's AppleGraphicsDeviceControl policy queries for the card | Original, against reverse-engineered AGDC headers | D: `drv/kexts/NVRMAGDC/Info.plist:14-17`, `nvrm-agdc.cpp:1-30` |
| **NMHDA** | Lilu plugin. Adds NVIDIA HDA IDs to AppleHDAController through `HwFactoryPrefixTranslation`. It matches by vendor `0x10DE` and class `0x0403xx`, not by a device-ID list, so it covers the 5090's `10DE:22E8` function. | Original, Lilu API | D: `drv/kexts/NMHDA/nmhda.cpp:47-63,84-86,99-121` |
| **NMIntelFB** | Framebuffer that adopts the pipe the firmware already lit on an Intel iGPU with display version 12-20. It does no mode set. NVIDIA renders the frames and NMIntelFB copies them in as NVAccel head 4. | Original. Register layout from Linux i915/xe (MIT). PWM method from WhateverXe. | D: `drv/kexts/NMIntelFB/intelfb.cpp:1-11,275-391`, `NOTICE:23-26` |
| **NVPrimarySelector** | Opt-in. Picks the one NVIDIA GPU that owns the firmware console and sets `nullmoth-auto-primary`. NVRM and NVAccel then match on that property. | Original | P: `drv/docs/AUTO-PRIMARY-GPU.md:1-16`. D: `package/install.sh:268-286` |
| **NMDRM** | `/dev/dri` nodes so Mesa RADV/ANV can enumerate AMD/Intel GPUs. Matches any display-class PCI device under its own match category. Only the core ioctls are implemented. | Original | D: `drv/kexts/NMDRM/nmdrm.cpp:1-12`, `Info.plist:14-19` |
| **Metal plugin** (`plugin/`) | `MTLDevice`, queues, encoders, resources, argument buffers, RT and mesh, on top of `nvmtl_vk.c`. Registers the VideoToolbox decoders and encoders. Optional vendor SASS path (`NVMTLVendorCompiler.m`, `GB20 -> sm_120`). | Original | D: `drv/plugin/NVMTLDevice.m:189-520`, `NVMTLVendorCompiler.m:59` |
| **Translator** (`translator/`) | Translates Apple AIR into SPIR-V | Fork of steelbrain/metal2vulkan (LGPL-3.0-or-later) | D: `drv/NOTICE:5-9`, `translator/translator/README.md:1-12` |
| **NVK** (`nvk/`) | Mesa's Vulkan driver plus the NAK compiler. It talks to RM through a new `nvkmd/nvrm` backend. The `nv-haiku.h` file name suggests that backend derives from the Haiku NVRM port (H). Copy channel `COPY4`, graphics fallback. `has_video=false`. | Mesa `17ca6174` + 12.3k-line `nvk-macos.patch` | D: `drv/nvk/MESA_BASE`, `nvk-macos.patch:7093-7113,8771,10197-10210,10590-10660` |
| **GSP firmware** | `gsp_ga10x.bin` and `ucodes_ga10x.bin` for Ampere and newer, `*_tu10x` for Turing. The kext checks SHA-1 and size. | NVIDIA 615.78.08, unmodified (P) | D: `drv/kexts/NVRM/os-xnu2.cpp:1199-1204,1221-1260` |
| **Video codecs** | `libnvdec_h264.dylib`, `libnvdec_hevc.dylib`, `libnvenc_*` (`_ga10x`, `_tu10x`), `libnvdec_h264_t2d.dylib` | **Closed: no source in the repo.** Only the loader logic and a byte patcher (`tools/make_nvdec_t2d.py`) are visible. | D: `git ls-files` has no codec source. `plugin/NVMTLDevice.m:290-320,406-430`. #105 says "The encoder libraries have no source in the repo" (P). |
| **Installer** | `package/install.sh` copies files, rebuilds the AuxKC and gates on `SUPPORTED_IDS`. The 1401 app also edits OpenCore. | Original | D: `drv/package/install.sh:10,150-168` |

## 3. Build provenance gap (affects every claim about "what runs")

- **D:** The NVIDIA RM and NVKMS core objects linked into NVRM.kext are not built by any script in the repo. `kexts/NVRM/rmcc.py:6` refers to a `build-nvrm.sh` that `git ls-files` does not contain. `accel_build.sh:4,13` needs `$OGKM/src/nvidia/_out/Darwin_x86_64/compile_cmds.sh`, which a stock tree does not have. Issue #51 comments say the reporters had to generate it.
- **D:** Version references are mixed. README and NOTICE say 615.78.08, and CHANGELOG 2.5.0 (`CHANGELOG.md:103`) says it moved from 610.57.04. But `accel_build.sh:4` defaults to `~/ogkm610`, `patches/README.md:13,23` targets 610.57.04, `build-completion-archive.py:33` stamps `610.57.04`, and `tools/test_copy_class_selection.py:43` snapshots 610.
- **H:** From source alone you cannot tell which RM version the shipped `NVRM` binary contains. The GSP image has to match the RM core's RPC version, and the kext pins its firmware by hash (`os-xnu2.cpp:1199-1204`). A mismatch would fail GSP boot, not run in a degraded mode. Check this on the Mac with `strings NVRM | grep 'Open Kernel Module'` against the installed `nvfw` hashes.
- **D:** The structs and ids that A1's patch depends on are identical in 610.57.04 and 615.78.08: `NV2080_CTRL_CMD_GPU_GET_ENGINES_V2=0x20800170`, `NV2080_GPU_MAX_ENGINES_LIST_SIZE=0x54`, and the NVDEC/NVENC engine ids. Checked in `ogkm610` (fetched) `ctrl2080gpu.h:778-781` and `cl2080_notification.h:303-353`, against `ogkm615` `ctrl2080gpu.h:779-789` and `cl2080_notification.h:306-355`.

## 4. Matching and claim order on a box with RTX 5090 + Arrow Lake iGPU

| Device | Personalities that can match | Result |
|---|---|---|
| `10DE:2B85` (class 0x0300) | NVRM (`IOPCIPrimaryMatch 0x000010de&0x0000ffff`, class 0x03), NVRMFBClaim (score 30000, IOFramebuffer category), NVRMBootFB (25000), NVAccel (IOAccelerator category), NMDRM (own category) | One NVRM instance, plus a framebuffer claim that keeps IONDRV off (D: plists cited above) |
| `10DE:22E8` (HDA) | AppleHDAController via NMHDA hooks | D: matched by class/vendor |
| `8086:7D67` | NMIntelFB (`IOPCIMatch 0x8086`, class 0x03, **score 60000**, IOFramebuffer), NMDRM; otherwise IONDRVFramebuffer | NMIntelFB is **not** in the default package (`install.sh:10` installs only NVRM, NVAccel, NVRMFB and NVRMAGDC). See 2b85-trace.md section 4. |
