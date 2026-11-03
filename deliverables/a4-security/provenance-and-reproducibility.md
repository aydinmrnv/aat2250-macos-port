# Provenance, source→binary traceability, licensing: v2.6.6

Agent A4, 2026-11-02. Labels: DEMONSTRATED / PROJECT CLAIM / HYPOTHESIS.
Release assets were downloaded to `src\dl-releases\` and the driver tarball was extracted to `dl-releases\x-driver\` for static reading only. Nothing was executed.

## 1. Repository metadata (DEMONSTRATED via git / `gh api`, 2026-11-02)

| | nvidia-macos-driver | 1401 |
|---|---|---|
| Owner | `nullmoth` (GitHub *User* account, not an org) | same |
| Created | 2026-10-07T08:13:12Z | 2026-10-07T08:13:16Z |
| Stars / forks | 4015 / 392 | 1149 / – |
| License field (GitHub) | NOASSERTION (PolyForm NC 1.0.0 in LICENSE) | NOASSERTION (PolyForm NC 1.0.0) |
| Commits at pin | 695 (548 by "NullMoth Systems"; 47 merges) | 436 (431 by "NullMoth Systems") |
| GitHub contributors | 33 | 3 |
| Signed commits (`%G?`) | **0 / 695** (all `N`) | **0 / 436** |
| Tags | 75; lightweight except annotated `v2.3.7` (unsigned) | 78; lightweight |
| Release cadence | v1.0.0 on 2026-10-07 to v2.6.6 on 2026-10-31: about 3 tags/day; up to 73 commits/day (2026-10-14) | same cadence, paired versions |
| CI workflows | **none** (`.github` holds only issue/PR templates) | `windows-candidate.yml`, `hardware-capture-validation.yml` |
| History | Commit d65dc39 "negative controls name their commits after the history rewrite". Hashes cited in #51 (b9a5a2b = v1.1.1, e7b900d) still resolve. | same message (a557bbc) |
| External PRs | Merged by the owner and re-committed with committer "NullMoth Systems" (author ≠ committer for contributors) | — |

Release v2.6.6 (driver): created 2026-11-01T01:54:15Z and published 02:01:00Z by `nullmoth`. All assets were uploaded by `nullmoth` at 02:00:45-46Z, and `target_commitish` = 470e391e. No CI exists for this repo, so **the release artefacts were built off-CI on a private "build Mac" and uploaded manually**. Evidence: build/build_plugin_release.sh:9-11 and build/build_xlate_release.sh:7-11 require `NULLMOTH_BUILD_HOST` (ssh) and `NULLMOTH_BUILD_ROOT`.

Release v2.6.6 (1401): published 02:11:17Z. `1401-Windows-2.6.6.zip` was uploaded 02:11:06Z by `nullmoth`. CI run 41263830256 (`workflow_dispatch`, head f5a9254, branch `support-fixes-1008`, success, 02:06:54Z) produced artifact `windows-candidate` (285,096,744 B, digest `sha256:425c9f0182aa…`). An earlier run on the same commit, 41263432694, produced 285,096,834 B (digest `17d24c44…`). The two runs on the same commit differ in size, so the "deterministic candidate archive" was not bit-identical across runs, or the artifact holds varying evidence data (HYPOTHESIS). Most likely the Windows zip was **CI-built, then hand-uploaded**. Not verified, because the artifact was not downloaded.

## 2. Release asset verification (DEMONSTRATED)

| Asset | Size | sha256 (local) | GitHub API `digest` | SHA256SUMS.txt | Ed25519 `.sig` (key `ztPmcj+m…d4E=`) |
|---|---|---|---|---|---|
| nullmoth-nvidia-2.6.6.tar.gz | 264,205,469 | dbb0f5c2a18ccc97dfdf57ca14488b0ea713c15608f684a0a4283e88c676ab6e | match | match | **Verified** (OpenSSL 3.5.4) |
| 1401-Mac-2.6.6.zip | 2,557,826 | 8e8f7b60a6b8410c93e01faccd96575dd7e9e55116001dbd9bb27c40e0fbd3c3 | match | match | **Verified** |
| 1401-Mac-2.6.6.dmg | 2,987,103 | 9b8876fb63533d7e1b19aaba1a0d210df52f1d214a9f2c36dff6d2dc2d079ca4 | match | match | **no .sig published** |
| 1401-Windows-2.6.6.zip | 284,977,673 | a7648ccb466810c6851291eba0bdea92abee30029355751a8ac4d82f01503f67 | match | match | **Verified** |
| driver SHA256SUMS.txt | 453 | 9932799274317c3614cfccfcad6d8d4b30f3bd2b0c3862fc0703b72a6849ce6c | match | — | unsigned |
| 1401 SHA256SUMS.txt | 90 | 958508d65eb150d18c4da1ef3e26f88779cef86841c70d7305300fc36de3ba4f | match | — | unsigned |

- The Windows zip embeds `NullMoth/nullmoth-nvidia-2.6.6.tar.gz` (sha256 dbb0f5c2…) and `NullMoth/1401-Mac-2.6.6.zip` (8e8f7b60…). Both are byte-identical to the driver release.
- The Windows zip's `BUILD-MANIFEST.json` records `source_commit` f5a9254… and pinned `release_inputs`.
- The inner package `SHA256SUMS` (74 lines) verifies against every extracted file. It does not cover the two `libvulkan*.dylib` symlinks or itself.

## 3. Inner package consistency problems (DEMONSTRATED)

- `pkgroot/MANIFEST.txt` is **stale**. It lists firmware under `nvfw/nvidia/610.57.04/`, but the package ships `615.78.08/`. It omits `libnvdec_h264_t2d`, `libnvenc_*_{ga10x,tu10x}`, `sass/` and `licenses/`.
- `pkgroot/BASELINE-IDENTITY.json` is **stale**. Its `scope` text describes "1.1.0 plus …" and says "installs only on macOS 15". Its `runtime_qualification` says RTX 5060 on 15.8.1. Its `paired_kexts` hashes do match the shipped kexts.
- Mixed NVIDIA versions: the kernel RM is 615.78.08 (NVRM strings: "NVIDIA UNIX Open Kernel Mode Setting Driver for x86_64 615.78.08 Release Build (NullMoth Systems)"). The NVVM and PTX JIT libraries come from **610.57.04**, as do the NVKMS completion patches (kexts/NVRM/patches/README.md).
- `Library/Extensions/NVAccel.kext` is identical to `kexts/15/NVAccel` (UUID 14cae207…). The `kexts/26` build differs (UUID 3d45dc36…).

## 4. Code signing of shipped binaries (static parse of LC_CODE_SIGNATURE; DEMONSTRATED)

| Binary | Signature | Identifier | Team ID | CMS |
|---|---|---|---|---|
| NVRM.kext/NVRM | ad-hoc (flags 0x2), CodeDirectory v0x20100 | com.nullmoth.NVRM | none | empty |
| NVRMFB, NVRMAGDC, NVAccel (15 and 26) | **unsigned** (no superblob; no _CodeSignature) | — | — | — |
| NVMTLDriver.bundle/NVMTLDriver | ad-hoc, CD v0x20400 | com.nullmoth.NVMTLDriver | none | empty |
| NVIDIAShared.bundle/NVIDIAShared | ad-hoc | com.nvmtl.NVIDIAShared | none | empty |
| libnvmtl_translate, libvulkan_nouveau, libLLVM, nvvmdrive | ad-hoc | hash-derived ids | none | empty |
| **nvmtl/air-opt** | **Apple-signed** (flags 0x0, full CMS, Apple Code Signing CA) | **com.apple.air-opt** | **59GAB85EFG** | 4442 B |

`install.sh:421` agrees: "only NVRM is signed in the package". No Developer ID is used anywhere, and nothing is notarised. Release authenticity rests entirely on the detached Ed25519 key.

Toolchain fingerprints from LC_BUILD_VERSION (DEMONSTRATED):

| Binary | SDK | minos | Linker |
|---|---|---|---|
| NVMTLDriver | 26.5 | 15.0 | ld 1267 (Xcode 26.x) |
| libnvmtl_translate | 26.5 | min 10.12 | — |
| libvulkan_nouveau, libvulkan loader, NVIDIAShared, nvvmdrive | 15.5 | — | ld 1167.5 (Xcode 16.4-era) |

So **at least two Xcode generations** were used. The README says "Xcode 16". Kexts carry no build-version load command. Per build_xlate_release.sh:23, the translator was cross-built with Rust 1.98.1 on an aarch64 host (Apple Silicon).

## 5. Source→binary traceability for v2.6.6

| Shipped artefact | Source in repo @470e391? | Build script? | Traceable? |
|---|---|---|---|
| NVRM.kext (17.5 MB, contains NVIDIA RM `libnvkernel.a` + NVKMS) | Shim sources yes (kexts/NVRM/*.cpp). RM comes from open-gpu-kernel-modules 615.78.08 **plus an unpublished Darwin port of its build** (`_out/Darwin_x86_64/compile_cmds.sh`, `libnvkms.a`). | **No.** rmcc.py:2-6 refers to `build-nvrm.sh`, which is absent from the tree and from all history (`git log --all -- '*build-nvrm*'` is empty). | **No.** DEMONSTRATED |
| NVRMFB.kext | kexts/NVRMFB/fb/*.cpp | **No script** in build/ | No |
| NVRMAGDC.kext | kexts/NVRMAGDC | build/build_agdc.sh reads from `$HOME/nvmtl-build/nm/agdc`, not the repo | Partial |
| NVAccel.kext (15/26) | kexts/NVRM/accel | build/accel_build.sh, accel_build_all.sh (`OGKM` defaults to `$HOME/ogkm610`, i.e. 610 headers) | Partial. Needs an RM tree with a Darwin `compile_cmds.sh`. Issue #51 shows third parties rebuilt NVAccel with functionally equivalent but **not byte-identical** results (301/324 identical functions). |
| NVMTLDriver.bundle | plugin/ | build/build_plugin_release.sh (remote build host; Vulkan headers from `$BASE/build/gpu-src/mesa/include`) | Partial (host-specific) |
| libnvmtl_translate.dylib | translator/ (LGPL-3.0) + Cargo.lock | build_xlate_release.sh (Rust 1.98.1) | Best candidate for reproduction |
| libvulkan_nouveau.dylib | nvk/nvk-macos.patch on Mesa `17ca6174` | build/build263.sh: hard-codes `$HOME/nvmtl-build/rebase263/...`, a Python venv, glslang, `$HOME/bin`, `-Dmesa-clc=system` (host mesa_clc unspecified) | Partial |
| libvulkan.1.4.362.dylib (Khronos loader) | none | none | **No** |
| NVIDIAShared.bundle (NVIDIAShared, nvvmdrive, air2nvvm.py, nvcompile.sh) | **none** (only mentioned in docs and plugin strings) | none | **No** |
| libnvidia-nvvm.nocanary.so, libnvidia-ptxjitcompiler.so.610.57.04 | NVIDIA proprietary Linux ELF. The "nocanary" library is **binary-patched** per nvcompile.sh:10-11 | none | **No** (third-party, modified) |
| libLLVM.dylib ×2 ("Homebrew LLVM version 21.1.8"), libzstd | Homebrew bottles | none | No |
| air-opt | Apple (com.apple.air-opt, Apple LLVM 32023.917.2) | none | No (Apple binary) |
| sass/*.bin, *.json | none | none | No |
| GSP/ucode firmware | NVIDIA 615.78.08 driver (PROJECT CLAIM) | none | Hash-pinned in kext source only |
| install.sh, uninstall.sh, nullmoth-runtime-check(.sh) | package/ | packaging script **not in repo** (no tar/SHA256SUMS/sign step) | install.sh traceable; package assembly not |
| 1401-Windows zip | 1401 @f5a9254 | CI workflow + tools/windows_release.py. **Starts from the previous binary release `1401-Windows-1.14.3.zip` as a baseline overlay** (windows_release.py:158-160). 1401.exe, p1401, upstream and the cpu helper are rebuilt; any file not replaced persists from the baseline. | Mostly traceable (CI log + artifact) |
| 1401-Mac app zip/DMG | 1401 mac/ (build.sh) | Built off-CI; contains AppleDouble `._*` files (zipped on a Mac without ditto) | Partial |

Known precedent, issue #51 (https://github.com/nullmoth/nvidia-macos-driver/issues/51): from 1.1.0 to 1.13.0 the `kexts/26/NVAccel` binary (`5aee66f5…`) was **not rebuilt from the tagged source**. A contributor also showed the official 1.13.0 `kexts/15/NVAccel` lacked symbols present in the v1.13.0 source ("built from a slightly different tree than the tag"). The maintainer says it was fixed in 1.14.0 (PROJECT CLAIM). This is DEMONSTRATED history of shipped binaries not matching tagged source.

**Verdict on traceability:** NOT traceable end-to-end. Only the translator, plugin, NVK and NVAccel have in-repo build paths, and all of those depend on the private build host. The main kext (NVRM), NVRMFB and the whole NVIDIAShared compiler lane have no public build recipe.

**Verdict on reproducibility:** NOT reproducible from public inputs today. Bit-for-bit reproduction is unlikely even for the parts with scripts: cargo path hashing (noted by the project), two Xcode generations, Homebrew-sourced libraries, and no recorded toolchain versions.

## 6. Pinned inventory

| Component | Source URL | Commit / tag / version | License | Hash (if known) |
|---|---|---|---|---|
| nvidia-macos-driver | https://github.com/nullmoth/nvidia-macos-driver | 470e391e8cf8d7e499cdabfa7f2daadb6e14bcfd (v2.6.6) | PolyForm-NC-1.0.0 (+LGPL translator/, MIT nvk/) | tarball dbb0f5c2…ab6e |
| 1401 | https://github.com/nullmoth/1401 | f5a9254079601d69a2e165fcd9f659e4f882bab9 (v2.6.6) | PolyForm-NC-1.0.0 | Win zip a7648ccb…3fe3; Mac zip 8e8f7b60…c3c3; DMG 9b8876fb…9ca4 |
| OpenCorePkg (local clone) | https://github.com/acidanthera/OpenCorePkg | 8d8d690cb543f63114340cdae958de4ce7a8296f (1.0.8+20) | BSD-3-Clause | — |
| OpenCore release used by 1401 CI | …/releases/download/1.0.8/OpenCore-1.0.8-RELEASE.zip | 1.0.8 | BSD-3-Clause | 2011e8b7216ecb2645d97ea710df965947edd406846e92050b9bc4190be6d27b |
| NVIDIA open-gpu-kernel-modules | https://github.com/NVIDIA/open-gpu-kernel-modules | tag 615.78.08 (tag obj 4bfe30bb78e7…, released 2026-10-07) | MIT / GPL-2.0 dual | not recorded by project |
| OGKM 610.57.04 (NVKMS patches, accel headers default) | same | 610.57.04 | MIT / GPL-2.0 | — |
| Mesa (NVK) | https://gitlab.freedesktop.org/mesa/mesa | `17ca6174` (short; binary says 26.3.0-devel); patch nvk/nvk-macos.patch (12,298 lines) | MIT | not resolvable here (GitHub mirror 404) |
| metal2vulkan (translator base) | https://github.com/steelbrain/metal2vulkan | unpinned | LGPL-3.0-or-later | — |
| Rust toolchain | rustup | 1.98.1 (aarch64 host → x86_64 target) per build_xlate_release.sh:23 | MIT/Apache | — |
| Translator crates | translator/wrapper/Cargo.lock | locked | various | — |
| GSP firmware | NVIDIA driver 615.78.08 (PROJECT CLAIM) | gsp_ga10x / gsp_tu10x / ucodes_ga10x / ucodes_tu10x | NVIDIA "License For Customer Use of NVIDIA Software" (as shipped) | SHA-1 2a484050…, 44efed1a…, f9bf8077…, 81b4f159… (match kext table); SHA-256 a20100ca…, 0c6124c1…, eed53d76…, 6027ab37… |
| libnvidia-nvvm.nocanary.so | NVIDIA 610.57.04 Linux driver (modified) | NVVM 7.0.1 | NVIDIA proprietary | 993f398a…fbc7 |
| libnvidia-ptxjitcompiler.so.610.57.04 | NVIDIA 610.57.04 Linux driver | CUDA 13.3 V13.3.80 | NVIDIA proprietary | 0df88a5d…bc7d |
| air-opt | Apple Xcode / Metal toolchain | Apple LLVM 32023.917.2 | Apple (Xcode SLA) | 8c41049c…6922 |
| libLLVM.dylib | Homebrew llvm | 21.1.8 | Apache-2.0 WITH LLVM-exception | — |
| Vulkan loader | KhronosGroup/Vulkan-Loader | 1.4.362 | Apache-2.0 (**no licence text shipped, not in NOTICE**) | dc81d049…d584 |
| libzstd | facebook/zstd | unknown | BSD-3 | — |
| Python (Win runtime) | python.org | 3.13.17 embeddable | PSF | 9e35319b…db38 |
| pywin32 / WMI / certifi | PyPI | 312 / 1.5.1 / 2026.10.23 | PSF / MIT / MPL-2.0 | pinned in BUILD-MANIFEST |
| .NET SDK | Microsoft | 8.0.425 | MIT | — |
| 1401 Windows baseline | …/1401/releases/download/v1.14.3/1401-Windows-1.14.3.zip | v1.14.3 | PolyForm-NC | e287bf4b…43b6 |
| OpCore-Simplify / Hardware-Sniffer | github.com/lzhoang2801 | e5d8a9f5… / 6570f91c… | BSD-3-Clause | — |
| GSP ID table | 1401 p1401/nvidia_gsp_ids.json | derived from OGKM 615.78.08 supported-gpus.json (PROJECT CLAIM); 247 IDs, tested: only 2D05 | — | — |

## 7. Firmware provenance

- **Where it comes from:** bundled in the driver tarball (`pkgroot/Users/Shared/nvfw/nvidia/615.78.08/*.bin`). `install.sh` copies it to the root-owned `/Library/NullMoth/nvfw` (install.sh:402-405). It is **not fetched from NVIDIA** at install or runtime. The 1401 apps ship `nullmoth-gpu-firmware.txt` (76 KB, not inspected) and `nvidia_gsp_ids.json`, which only gates supported device IDs.
- **Digest check:** the kext checks size and SHA-1 for the 4 shipped files (kexts/NVRM/os-xnu2.cpp:1196-1256). The shipped files match the table (DEMONSTRATED). `gsp_log_*` is not covered. That the images are byte-identical to NVIDIA's 615.78.08 `.run` firmware is a **PROJECT CLAIM I could not verify** (the .run download was not in scope). To verify: extract `NVIDIA-Linux-x86_64-615.78.08.run --extract-only` → `firmware/gsp_*.bin` and compare SHA-256.
- **Licence:** NOTICE:17-19 claims redistribution "under NVIDIA's licence … the same terms under which linux-firmware redistributes these images". The text actually shipped (licenses/NVIDIA-firmware-LICENCE.txt) is NVIDIA's *License For Customer Use of NVIDIA Software*. Its §2.1.2 "Open Source Exception" allows redistribution "solely for use on operating systems distributed under the terms of an OSI-approved open source license … provided that the binary files thereof are not modified". macOS is not such an OS. **HYPOTHESIS (legal): redistributing GSP firmware for macOS falls outside that grant.** The NOTICE claim also misstates which licence applies.

## 8. Licensing analysis

| Item | Finding | Label |
|---|---|---|
| PolyForm NC 1.0.0 (driver code, 1401) | Personal, hobby, research and non-profit use is permitted. Any commercial use is not, which includes using it on a work machine for a for-profit purpose (PolyForm NC "Personal Uses" / "Noncommercial Organizations"). Redistribution must carry the licence and "Required Notice:" lines. PolyForm is not OSI-approved. **For this project's personal homelab use: OK.** | DEMONSTRATED (text) |
| NVIDIA OGKM (MIT/GPL-2.0 dual) linked into PolyForm kext | Taking the MIT option allows sublicensing, so combining it with PolyForm-NC shim code is permissible. Conflict arises only if any GPL-2.0-only file (no MIT option) is linked. I did not audit the per-file RM headers. | HYPOTHESIS: no conflict if MIT elected |
| translator/ LGPL-3.0-or-later | Shipped as a separate dylib (`libnvmtl_translate.dylib`, relinkable) loaded by the PolyForm plugin. The LGPL/GPL texts are shipped. The source is published. | Compatible as structured (HYPOTHESIS) |
| Mesa NVK (MIT) | Compatible. Mesa-license.rst is shipped. | OK |
| WhateverXe (BSD-3) method in NMIntelFB | Attribution present | OK |
| **NVIDIA proprietary NVVM / PTX-JIT .so** | Not listed in NOTICE. Redistributed for macOS (outside the §2.1.2 OSI-OS exception). **Binary-modified** (`nocanary`; nvcompile.sh:10-11), which violates both "not modified" and "No Reverse Engineering". | Strong HYPOTHESIS of licence violation |
| **Apple `air-opt`** | Apple-signed Xcode/Metal toolchain binary redistributed. The Xcode and Apple SDKs Agreement does not permit redistributing Xcode components. | HYPOTHESIS of licence violation |
| Vulkan loader (Apache-2.0) | No licence text or NOTICE entry, which Apache-2.0 §4 requires | DEMONSTRATED omission |
| Homebrew LLVM (Apache-2.0+LLVM-exception) in NVIDIAShared | Licence text only in the NVMTLDriver air-runtime folder | Minor |
| 1401 bundles GPL-2.0/3.0 third-party kexts | Downloaded on the user's machine, pinned, and attributed (1401 NOTICE.md). PolyForm terms do not reach them. | OK (PROJECT CLAIM) |
| Practical risk to this project | Personal non-commercial use is within PolyForm. The redistribution issues are the publisher's liability, not the end user's. They do mean we should **not mirror or redistribute** the package (e.g. internally to other machines or users), and should note the takedown risk (NVIDIA/Apple) for availability planning. | HYPOTHESIS |
