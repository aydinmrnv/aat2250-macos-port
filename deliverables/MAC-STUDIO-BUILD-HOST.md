# Mac Studio as build and installer host (draft, 2026-11-02)

The user has a Mac Studio, which is Apple Silicon. It removes the "no macOS build host" blocker. It does **not** remove the need to test on the Alienware.

## What the Mac Studio unlocks

| Need | How the Mac Studio covers it | Status |
|---|---|---|
| Compile x86_64 kexts (A1 patch, a future VMD driver, VoodooHDA work) | Xcode builds x86_64 kexts on Apple Silicon (`-arch x86_64`, Kernel.framework from the macOS SDK) | HYPOTHESIS until a first kext compiles there |
| Rebuild the driver's translator | `build_xlate_release.sh` expects an Apple Silicon host with Rust `1.98.1-aarch64-apple-darwin` (`build/build_xlate_release.sh:23`) | PROJECT requirement |
| Rebuild plugin, NVK, NVAccel and compare with release hashes | Xcode 26.x (SDK 26.5) for plugin/translator; Xcode 16.4 (SDK 15.5) for NVK. Method in `a4-security/build-instructions.md` §5 | NVRM/NVRMFB still blocked: `build-nvrm.sh` is missing upstream |
| Apple-sourced Tahoe installer | `softwareupdate --list-full-installers`, then `--fetch-full-installer --full-installer-version 26.x` | Whether the full installer also boots Intel Macs must be verified before relying on it |
| `iasl` for SSDTs, `ocvalidate`, `plutil` | Homebrew `acpica`; OpenCore 1.0.8 macOS ocvalidate | Straightforward |
| Two-machine kernel debugging (Mac Studio → Alienware) | Needs a matching x86_64 Kernel Debug Kit and a debug transport the Alienware's NIC supports under KDP | HYPOTHESIS: KDP may not work over Realtek/LucyRTL; needs checking |

## What it cannot do
- Load or run x86_64 kexts. Its kernel is arm64.
- Host the RTX 5090. Apple Silicon Macs do not support NVIDIA GPUs or eGPUs.
- Stand in for any Alienware acceptance test (graphics, BAR1, sleep, audio, USB).

## Containment: build untrusted code in a VM, not on the main account
The driver's build scripts assume private paths, `ssh` to `NULLMOTH_BUILD_HOST`, and pull unpinned tools (A4). Run them inside a **macOS VM on the Mac Studio** (Virtualization.framework, e.g. UTM or `tart`). Apple's license allows macOS VMs on Apple hardware. Give the VM no access to the host's home folder and no Apple ID or keychain, and snapshot it before each build. Only the build outputs and hashes come out.

## Phased plan

Each phase needs the user's go-ahead. Nothing here has been run.

1. **Inventory (read-only):** `sw_vers`, `sysctl -n machdep.cpu.brand_string`, `df -h /`, and whether Xcode is installed (`xcode-select -p`, `xcodebuild -version`). Budget about 150 GB of free space for two Xcodes, Mesa and the VM.
2. **VM and toolchain:** create the Tahoe VM; install Xcode 26.x and 16.4 (Apple ID needed; the user signs in), Homebrew, rustup 1.98.1 with the `x86_64-apple-darwin` target, meson/ninja/glslang (record versions), and acpica.
3. **First compile:**
   - Syntax-check and build the A1 patch's `nvrm_fbinfo.cpp` and `NVRM.cpp` with the flag set from `kexts/NVRM/rmcc.py` (review that script first) against open-gpu-kernel-modules 615.78.08.
   - This is the first real compile of the patch. A full NVRM.kext link is still blocked on `build-nvrm.sh`.
4. **Functional rebuild of translator, plugin and NVK:** compare with the v2.6.6 hashes and per-function diffs (`a4-security/build-instructions.md` §5). This tests whether the release corresponds to the source.
5. **Installer USB:** fetch the Tahoe full installer and record its version and build. `createinstallmedia` **erases the target USB stick**, so it needs explicit approval and a verified blank stick.
6. **OpenCore EFI assembly:** build only after the A2 draft's UNRESOLVED values are filled. Generate the SMBIOS serials on the Mac and keep them out of logs and repos. Run ocvalidate.
7. **Custom driver work:** begin only after Tahoe boots on the Alienware from external media. Order: USB map → SSDTs → audio (VoodooHDA adaptation vs AppleHDA restore) → VMD driver (only if the user picks the internal-D: route).

## Information needed from the user
- The Mac Studio's chip, its macOS version, and its free disk space.
- How Claude should work on it:
  - (a) run Claude Code on the Mac Studio itself, or
  - (b) the user enables Remote Login and Claude connects from here by SSH key. The user creates and installs the key; Claude never handles a password.
- Whether it may hold a dedicated build VM.

## Inventory result (2026-11-02, read-only over SSH with a key, from `from=`-restricted LAN addresses)

**Machine**
- Mac Studio Mac13,1, M1 Max, 10 CPU cores, 32 GB RAM.
- macOS 27.0 "Golden Gate" (26A428), which is Apple Silicon-only.
- Hypervisor support: yes (`kern.hv_support`=1).

**Disk**
- **Only 62 GiB free** on the 460 GiB volume. The VM plan above (about 150 GB) does not fit. An external disk for the VM, or freeing space, is needed.

**Xcode**
- Xcode 27.0 (27A266a), SDK MacOSX27.0, with Kernel.framework headers present. Xcodes.app is installed, so older Xcodes can be added.

**Tools**
- Present: `clang`, `git`, `python3`.
- Missing: Homebrew, rustup/cargo, meson, ninja, iasl, tart, UTM.

**Tahoe installers offered by Apple**
- 26.7.2 (25G332), 26.7.1 and 26.7, each about 18 GB.
- 26.7.2 matches the build that the BE200 Wi-Fi alpha was tested on.

**DEMONSTRATED: compile test**
- A minimal IOService subclass compiled with `clang++ -target x86_64-apple-macos15.0 -mkernel -fapple-kext` against the SDK 27.0 Kernel.framework.
- The output is a `Mach-O 64-bit object x86_64`.
- So Xcode 27 on this Mac can compile x86_64 kext code.
- Linking a full kext bundle and loading it are **not** tested. Loading is impossible on this arm64 Mac.
- Temp files were deleted.

## Toolchain installed (2026-11-02, user-level, no sudo)
| Tool | Version | How |
|---|---|---|
| Xcode / SDK | 27.0 (27A266a) / MacOSX27.0 | pre-existing |
| iasl (ACPICA) | 20260408 | `brew install acpica` |
| meson | 1.12.1 (runs on Homebrew python@3.15 3.15.2) | `brew install meson` |
| ninja | 1.13.2 | `brew install ninja` |
| Rust | 1.98.1 (48a229cea 2026-09-01) + `x86_64-apple-darwin` target, minimal profile | `rustup toolchain install 1.98.1` |

Side effect and fix: installing meson pulled in python@3.15, and Homebrew re-pointed `/opt/homebrew/bin/python3` (and pip3, idle3, pydoc3, python3-config) from 3.14.7 to 3.15.2.
- **Restored:** `brew unlink python@3.15`, then `brew unlink python@3.14 && brew link --overwrite python@3.14`.
- Verified: `python3` = 3.14.7, and meson still works via its absolute 3.15 shebang.
- Homebrew's backup of the original links is at `~/Library/Caches/Homebrew/Backup`.
- The rustup default (`stable`) is unchanged.

Space: 30 GB of Xcode caches sit in `~/.Trash/claude-freed-2026-11-02`, not yet emptied. Permanent deletion and simulator runtime removal are left to the user.
