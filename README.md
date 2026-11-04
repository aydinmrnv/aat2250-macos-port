# aat2250-macos-port

This is an evidence-driven attempt to run **macOS Tahoe** on an **Alienware Area-51 AAT2250**: Core Ultra 9 285K (Arrow Lake), RTX 5090, Intel VMD storage, and a WD19TB Thunderbolt dock.

> **Status (2026-11-04): macOS Tahoe Recovery boots on the AAT2250** with the config in `efi/config.plist`. Installing macOS, internal storage (VMD), the RTX 5090, the iGPU and the dock are not done yet.
> - The first diagnostic boot is ready: see **[BOOTING.md](BOOTING.md)**.
> - A successful boot will not mean the machine is usable. Every component has its own acceptance tests in `deliverables/`.

## What's here
| Path | What |
|---|---|
| [BOOTING.md](BOOTING.md) | Exact steps to build the USB stick and run the diagnostic boot |
| `tools/build-efi.sh` | Builds the OpenCore EFI from pinned, SHA-256-checked releases plus this repo's kexts and ACPI. It generates SMBIOS locally. |
| `tools/get-recovery.sh` | Downloads the Tahoe recovery image from Apple, using OpenCore's `macrecovery.py` |
| `efi/config.plist` | The installer-test OpenCore config (SMBIOS fields are placeholders). `ocvalidate`: no issues. |
| `vmd-driver/` | **VMDProbe** (read-only) and **VMDBridge** (an IOPCIHostBridge for Intel VMD 0xAD0B, so macOS can see disks behind VMD). Includes source, build, design and reviews. |
| `usb4-driver/` | **USB4Probe** (read-only): phase D1 of a USB4 connection manager for the integrated Thunderbolt and the dock |
| `cuda-shim/` | Experimental: loads NVIDIA's **unmodified** Linux `libcuda.so` (615.78.08) into a macOS process through an ELF loader and glibc shim. Without a GPU, `cuInit` behaves as on Linux (returns `CUDA_ERROR_NO_DEVICE`). **No CUDA work has run on a GPU yet.** NVIDIA files are not included; see `deliverables/cuda/C0-REPORT.md`. |
| `deliverables/acpi/` | SSDT-PLUG-ALT, SSDT-EC-USBX-AAT2250, SSDT-AWAC-DISABLE (source and compiled) |
| `deliverables/` | Compatibility matrix, decisions, progress log, plans, test definitions, security review |
| `patches/` | Diagnostic patches for the third-party NVIDIA driver (RTX 5090 video-engine instances) |
| `evidence/` | Hardware facts gathered read-only from Windows; `evidence/cuda/` holds the Linux reference `strace` of `cuInit` and its harness |

## Honest expectations
Details are in [deliverables/COMPATIBILITY-MATRIX.md](deliverables/COMPATIBILITY-MATRIX.md).

**Credible paths:**
- wired Ethernet;
- Bluetooth (no Continuity);
- the read-only diagnostics.

**Hard problems:**
- internal disks behind VMD;
- Arrow Lake CPU support on Tahoe;
- RTX 5090 acceleration, which depends on an unreproducible third-party driver that needs SIP and AMFI reduced;
- the Intel iGPU monitors and the WD19TB dock (new drivers needed; see `deliverables/DOCK-AND-IGPU-PLAN.md`);
- CUDA (the loader works without a GPU; the device shim, a UVM kernel extension and a working 5090 driver are all still needed);
- sleep (the firmware only offers S0ix);
- analog audio on Tahoe.

## Building the kexts
Each driver has a `build.sh` that needs Xcode on any Mac (it cross-compiles x86_64 kexts on Apple Silicon). Prebuilt, ad-hoc-signed copies are in each `dist/`.

## Licences and third-party material
- `vmd-driver/VMDBridge/IOPCIHostBridgeMirror.h` mirrors a declaration from Apple's IOPCIFamily, which is under the **Apple Public Source License 2.0**.
- `patches/` modify nullmoth/nvidia-macos-driver, which is under the **PolyForm Noncommercial 1.0.0** licence. The patches are offered under that licence's terms, for noncommercial use.
- FreeBSD `vmd.c` (BSD-2) was the design reference for VMD. Linux `vmd.c` and `drivers/thunderbolt` (GPL) were read as references only, and no code was copied.
- `cuda-shim` runs NVIDIA's Linux driver library unmodified. NVIDIA's licence forbids disassembling or modifying its binaries, so this project never does either: it uses only ELF metadata, NVIDIA's open-source headers, and behaviour observed at the C-library and system-call interface. You download NVIDIA's package yourself, and whether running it this way fits that licence is your own judgement.
- No third-party binaries are committed. `tools/build-efi.sh` downloads them from their official releases.
- The project's own code does not have a licence chosen yet.

**Not affiliated with Dell, Alienware, Apple, Intel or NVIDIA.** Use at your own risk.
