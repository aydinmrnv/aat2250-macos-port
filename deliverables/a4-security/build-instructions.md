# Reproducible-build plan: NullMoth NVIDIA driver v2.6.6 + 1401 v2.6.6

**Status:** I had no macOS build host. Nothing below has been executed. It is a plan derived from static reading of build/*.sh, kexts/*/build.sh and the shipped binaries' load commands, all @470e391e8cf8d7e499cdabfa7f2daadb6e14bcfd.

**Bottom line:**
- A full, bit-identical reproduction of `nullmoth-nvidia-2.6.6.tar.gz` is **not possible from public inputs**. Inputs that are missing:
  - the NVRM/NVRMFB build script (`build-nvrm.sh` is referenced by kexts/NVRM/rmcc.py:6 but is absent)
  - the Darwin build port of open-gpu-kernel-modules (`_out/Darwin_x86_64/compile_cmds.sh`, `libnvkernel.a`, `libnvkms.a`)
  - the package assembly and signing script
  - the sources for NVIDIAShared/nvvmdrive
- What *can* be attempted is a **functional-equivalence rebuild** of: translator, plugin, NVK, and (given an RM Darwin tree) NVAccel. The comparison method is in §5.

## 1. Host requirements (derived; DEMONSTRATED from binaries unless noted)

| Item | Requirement | Evidence |
|---|---|---|
| Hardware | Apple Silicon Mac for the translator (Rust toolchain `1.98.1-aarch64-apple-darwin`); x86_64 output. An Intel Mac works for the clang parts. | build/build_xlate_release.sh:23 |
| macOS | 15.5 or later host (26.x preferred to match SDK 26.5) | LC_BUILD_VERSION |
| Xcode for plugin + translator | **Xcode 26.x with macOS SDK 26.5** (ld-1267), deployment target 15.0 | NVMTLDriver: sdk 26.5, ld 1267, minos 15.0 |
| Xcode for NVK / loader / NVIDIAShared | **Xcode 16.4-era, SDK 15.5** (ld-1167.5) | libvulkan_nouveau, nvvmdrive: sdk 15.5 |
| Kexts | `xcrun --show-sdk-path` Kernel.framework headers. Issue #51 rebuilds used Apple clang 21.0.0 (clang-2100.3.34.2) with SDK 26.5/27.0, which gave byte-identical output across those SDKs. | issue #51 comments (PROJECT/third-party claim) |
| README claim | "Xcode 16, Rust (stable), Meson/Ninja" | README.md:155 (incomplete: binaries show two Xcode generations) |
| Rust | 1.98.1 exactly; `cargo build --release --target x86_64-apple-darwin`; Cargo.lock at translator/wrapper/Cargo.lock | build_xlate_release.sh |
| Meson / Ninja | **Unpinned.** Use the Mesa 26.3-devel-era minimum (meson ≥1.4 assumed; HYPOTHESIS). Record the versions. | build263.sh |
| Python | Python 3 venv at `$HOME/nvmtl-build/rebase263/py` (contents unknown; Mesa needs mako, pyyaml, packaging, plus pycparser for NAK) | build263.sh:3 |
| Others | glslang (`$HOME/tools/glslang/bin`), a system `mesa_clc` (`-Dmesa-clc=system`), cargo, Vulkan-Headers (`$HOME/Vulkan-Headers` or the Mesa include dir), Homebrew | build263.sh, build_plugin*.sh |
| NVIDIA sources | open-gpu-kernel-modules **615.78.08** (README), **plus 610.57.04** (accel_build.sh default `~/ogkm610`; NVKMS patches are 610-based) | accel_build.sh:4; kexts/NVRM/patches/README.md |
| Mesa | mesa @ `17ca6174…` (short hash; resolve to full SHA on gitlab.freedesktop.org/mesa/mesa first) | nvk/MESA_BASE |

## 2. Fetch pinned inputs

```bash
git clone https://github.com/nullmoth/nvidia-macos-driver && git -C nvidia-macos-driver checkout 470e391e8cf8d7e499cdabfa7f2daadb6e14bcfd
git clone https://github.com/NVIDIA/open-gpu-kernel-modules ogkm615 && git -C ogkm615 checkout 615.78.08
git clone https://github.com/NVIDIA/open-gpu-kernel-modules ogkm610 && git -C ogkm610 checkout 610.57.04
git clone https://gitlab.freedesktop.org/mesa/mesa && git -C mesa checkout 17ca6174   # verify it resolves uniquely; record full SHA
rustup toolchain install 1.98.1 --target x86_64-apple-darwin
```

## 3. Build steps, in README order (README.md:157-167)

1. **Translator.** Use the release variant, not `build_xlate.sh` (that one builds from `$HOME/nvmtl-build/bindless`).
   ```bash
   export NULLMOTH_BUILD_HOST=localhost NULLMOTH_BUILD_ROOT=$HOME/nm-repro   # the script ssh'es; localhost works if sshd is on
   build/build_xlate_release.sh 470e391 out/xlate
   ```
   It applies `--remap-path-prefix`, `strip -S`, `install_name_tool -id @rpath/...` and ad-hoc `codesign`. Keep `$R` fixed (`$BASE/build/xlate-release`) because cargo hashes the path into symbol names (script comment, lines 18-19), so the build root must equal the original to reach byte identity. **The original value is unknown** (blocker).
2. **Plugin.**
   ```bash
   build/build_plugin_release.sh 470e391 out/plugin
   ```
   Needs `$NULLMOTH_BUILD_ROOT/build/gpu-src/mesa/include` (Vulkan headers from the Mesa tree). The script checks minos 15.0.
3. **NVK.**
   ```bash
   cp -a mesa ~/nvmtl-build/rebase263/mesa
   rm -rf ~/nvmtl-build/rebase263/mesa/.git
   ```
   build263.sh refuses a `.git`. Then apply `nvk/nvk-macos.patch`:
   ```bash
   patch -p1 < nvk/nvk-macos.patch
   ```
   The paths are `upmesa/mesa-17ca6174/...` → `rebase263/mesa/...`, so adjust `-p`. Then run `build/build263.sh`.
   - `-Dbuildtype=debugoptimized`, nouveau only.
   - Output is `strip -S`'d.
   - `-ffile-prefix-map` is used, so the `$HOME` value is normalised.
4. **NVAccel (15 and 26).**
   ```bash
   OGKM=<ogkm tree with _out/Darwin_x86_64/compile_cmds.sh> build/accel_build_all.sh kexts/NVRM out/accel
   ```
   The stock OGKM tree has no Darwin `compile_cmds.sh`. Issue #51 contributors generated one with `make -n TARGET_ARCH=x86_64` in `src/nvidia` and filtered to the `-D` flags. That is a third-party workaround and not byte-faithful.
5. **NVRMAGDC.** Run `build/build_agdc.sh` after copying `kexts/NVRMAGDC` to `$HOME/nvmtl-build/nm/agdc` (the script hard-codes that path).
6. **NVRM.kext, NVRMFB.kext.** **No recipe exists.** This needs:
   - an OGKM Darwin build producing `libnvkernel.a`
   - `libnvkms.a`, patched via `kexts/NVRM/build-completion-archive.py --vendor <610 nvidia-modeset> --output ...`
   - compiling the shim with `kexts/NVRM/rmcc.py`
   - the missing `build-nvrm.sh` link step

   **Blocker. Ask the maintainer for build-nvrm.sh and the Darwin makefile changes.**
7. **NVIDIAShared.bundle, air-opt, Vulkan loader, Homebrew LLVM, NVVM/PTX libraries, sass/ blobs.** No sources. Take them from the release tarball. Verify by hash only.
8. **Package.** No script. Recreate the `pkgroot/` layout from the release tarball listing, generate `SHA256SUMS`, and compare per-file.

1401 (Windows) can be reproduced in CI-equivalent fashion:
- Use `windows-2022`, .NET SDK 8.0.425, VS C++ tools.
- Run:
  ```
  dotnet build windows/App/App.csproj -c Release -p:ContinuousIntegrationBuild=true
  python tools/windows_release.py prepare ...
  python tools/windows_release.py package ...
  ```
  This is exactly `.github/workflows/windows-candidate.yml` @f5a9254. It downloads pinned inputs (baseline 1401-Windows-1.14.3.zip, Python 3.13.17 embed, wheels, driver release assets) with sha256 checks. A *fork's* `workflow_dispatch` run is the cleanest independent rebuild. That runs project code, so it was out of scope for this agent.

## 4. Non-reproducible inputs to record or freeze

- `$HOME`-relative trees: `~/nvmtl-build/*`, `~/ogkm610`, `~/Vulkan-Headers`, `~/tools/glslang`, `~/.cargo`, `~/Library/Python/3.9/bin` (build263.sh:3; build_xlate.sh:3-4; build_agdc.sh:3; accel_build.sh:4).
- `NULLMOTH_BUILD_HOST` / `NULLMOTH_BUILD_ROOT` are private (build_*_release.sh).
- Unpinned versions: Xcode, meson, ninja, Python venv contents, glslang, `mesa_clc`, Homebrew LLVM.
- Prebuilt blobs: NVVM/PTX (610.57.04), air-opt, libLLVM, Vulkan loader, sass/*.bin, GSP firmware.
- The Mesa base is a short hash. The OGKM tag is given, but the Darwin port of it is not published.

## 5. Verifying output against the release

1. **Release level.** `sha256sum` each asset and compare with the GitHub API `digest` and `SHA256SUMS.txt`. Verify the Ed25519 `.sig` independently:
   ```bash
   (printf '\x30\x2a\x30\x05\x06\x03\x2b\x65\x70\x03\x21\x00'; echo ztPmcj+mcHYM1X2lrp24yjq0xBuzke7gh5XJ3E2fd4E= | base64 -d) > pub.der
   base64 -d nullmoth-nvidia-2.6.6.tar.gz.sig > sig.bin
   openssl pkeyutl -verify -pubin -inkey pub.der -keyform DER -rawin -in nullmoth-nvidia-2.6.6.tar.gz -sigfile sig.bin
   ```
   Done on 2026-11-02: all three signed assets verified.
2. **File level.** Extract the tarball and run `shasum -a 256 -c SHA256SUMS`. Compare the rebuilt files with these release values:

   | File | sha256 |
   |---|---|
   | NVAccel (15) | 5412917c3fbc13da8df6275a723bba397630288974d7d7dace52bde621388410 |
   | NVAccel (26) | de3222062e5d04d3ae4a20ca9c627e6d1707549f80752ade060e3b22b62eb532 |
   | NVMTLDriver | d877d83cb8618efcd898b4e496136475cadcc94a747546463379fa2d5074535a |
   | libnvmtl_translate | c48289bc20af549ef85a67d952c260ae5140aa38d5e1939f614f9a300b0704ff |
   | libvulkan_nouveau | 80d6018275f0ae9f7d9ded73b94b720e87d00f4e18125a9f3d213f8d988e2bc6 |

   Full values are in `dl-releases/x-driver/pkgroot/SHA256SUMS`.
3. **Ad-hoc signatures.** These embed hashes of the file. Before comparing, strip the signatures (`codesign --remove-signature`) on both sides, or compare `__TEXT` sections only.
4. **Functional equivalence when bytes differ.** This is the approach used in issue #51. Compare:
   - LC_UUID is expected to differ
   - imports and exports (`nm -gU`, `nm -u`)
   - per-function instruction diff with addresses normalised
   - key ABI constants (e.g. the `NVRM_VRAM_ABI_VERSION` store in `NVVidMemory::allocPhysical`)
5. **Firmware.** Extract NVIDIA-Linux-x86_64-615.78.08.run `--extract-only` and compare `firmware/gsp_{ga10x,tu10x}.bin` SHA-256 with a20100ca… / 0c6124c1…. The `ucodes_*` images must be checked against how the project derived them (not documented).
6. **1401 Windows.** Download the CI artifact of run 41263830256 before 2026-11-04 (`gh run download 41263830256 -R nullmoth/1401 -n windows-candidate`) and compare the inner zip's sha256 with a7648ccb…3fe3.
