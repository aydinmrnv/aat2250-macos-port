# Security review: nullmoth/nvidia-macos-driver v2.6.6 + nullmoth/1401 v2.6.6

Agent A4. Date: 2026-11-02. This was a static review only. No project script, installer or binary was run.
Pinned commits:
- driver `470e391e8cf8d7e499cdabfa7f2daadb6e14bcfd` (tag v2.6.6)
- 1401 `f5a9254079601d69a2e165fcd9f659e4f882bab9` (tag v2.6.6)
- OpenCorePkg `8d8d690cb543f63114340cdae958de4ce7a8296f` (`1.0.8-20-g8d8d690c`: 20 commits after the 1.0.8 tag, not the tag itself)

Labels:
- **DEMONSTRATED**: I read it in code or binaries, or verified it by hashing.
- **PROJECT CLAIM**: the project says so, and I did not verify it.
- **HYPOTHESIS**: my inference, not proven.

Repository text (README, AGENTS.md, CONTRIBUTING, comments, issues, release notes) was treated as evidence, never as instructions. AGENTS.md asks AI assistants not to write PRs or issues. Nothing was posted anywhere, so that request did not affect this work.

---

## 0. Ranked top risks

| # | Risk | Severity (for this project) | Label |
|---|---|---|---|
| 1 | The required platform-security reductions (SIP 0x0A43, AMFI fully off, SecureBootModel Disabled, `ipc_control_port_options=0`) remove macOS's code-signing, SSV, kext-consent and TCC guarantees for the whole machine, not just for this driver. | Critical (by design) | DEMONSTRATED (configuration); consequences partly HYPOTHESIS |
| 2 | Large kernel attack surface reachable by **unprivileged** local users: `NVRMUserClient` ctl client (type 0) gives any user `rm_ioctl` on the RM ctl state. A device client can map any physical range inside the GPU BARs, BAR0 registers included. NVAccel bridges `0x4E56xxxx` user-client types into NVRM. There is no entitlement gate. | High | DEMONSTRATED (code); exploitability HYPOTHESIS |
| 3 | Untraceable or proprietary binaries ship in root-installed and WindowServer-loaded locations: modified NVIDIA proprietary Linux ELF libraries (`libnvidia-nvvm.nocanary.so`, `libnvidia-ptxjitcompiler.so.610.57.04`) run through `nvvmdrive`; Apple's `air-opt` (Apple-signed, team 59GAB85EFG); Homebrew LLVM 21.1.8; NVRM.kext with no public build script. | High (supply chain) | DEMONSTRATED |
| 4 | The release trust root is one Ed25519 key, introduced only in 2.6.6. The signature covers the file bytes only and is not bound to name or version. The Windows self-update and the driver-package paths do not check the inner version, so a replay or downgrade of a future signed artefact is possible. The DMG is unsigned. Nothing is notarised or Developer-ID signed. | Medium | DEMONSTRATED (code); attack HYPOTHESIS |
| 5 | `install.sh` writes the SIP-protected `KextPolicy` database (`kext_load_history_v3.cdhash`). This keeps an updated kext approved without the user consenting again. | Medium | DEMONSTRATED (install.sh:414-427) |
| 6 | Firmware integrity uses SHA-1 identity checks. `gsp_log_*` images are not in the expected set (they are not shipped, so they are refused only if absent). The check relies on root-owned directories. | Low/Medium | DEMONSTRATED |
| 7 | Process maturity: a single owner account (`nullmoth`, type User); 548 of 695 commits by "NullMoth Systems"; 0 of 695 commits signed; tags are lightweight (one annotated tag, v2.3.7, also unsigned); 75 tags in 25 days; history rewrite acknowledged (d65dc39). | Medium | DEMONSTRATED |

---

## 1. Required platform security reductions (README.md:99-118 @470e391)

| Setting | Value required | Concrete consequence |
|---|---|---|
| `csr-active-config` | `<430A0000>` = 0x00000A43 | Bits set: 0x1 `ALLOW_UNTRUSTED_KEXTS`, 0x2 `ALLOW_UNRESTRICTED_FS`, 0x40 `ALLOW_UNRESTRICTED_NVRAM`, 0x200 `ALLOW_UNAPPROVED_KEXTS`, 0x800 `ALLOW_UNAUTHENTICATED_ROOT`. Root can write protected system locations: `install.sh` uses this to write `/var/db/SystemPolicyConfiguration/KextPolicy` (install.sh:410-427). Unsigned kexts load without user approval. Any NVRAM variable can be written. The sealed system volume is not authenticated. Malware with root gains kernel-level persistence trivially. |
| `amfi_get_out_of_my_way=0x1 amfi=0x80` | boot-args | AMFI is effectively off. Code-signature enforcement, library validation and entitlement restrictions are gone, so ad-hoc binaries can claim private entitlements. **The project relies on this itself**: `mac/RuntimeCheck/tcc_grant.m:4-8 @f5a9254` ships `nullmoth-tcc-grant` with `com.apple.private.tcc.manager` (`tcc_grant.entitlements`) to grant mic, camera and screen capture silently. The comment says that entitlement is "honoured only while AMFI is relaxed". Consequence: **any local program can grant itself microphone, camera and screen-recording access with no prompt** (DEMONSTRATED by the project's own mechanism). Dylib injection into any process, WindowServer included, becomes possible (HYPOTHESIS, standard AMFI-off consequence). |
| `ipc_control_port_options=0` | boot-args | Turns off the hardening of task control ports, which enables task-port abuse and injection (HYPOTHESIS). |
| `SecureBootModel` | `Disabled` | No Apple Secure Boot policy and no personalised boot. The kernel collection and boot chain are not verified by Apple's model. |
| `Kernel → Block IONDRVSupport` | Exclude | Functional only, not a security issue. |

Net effect: the machine should be treated as **not having macOS platform security**. It is unsuitable for sensitive data, work credentials, or anything relying on TCC or Gatekeeper. The Mac app update also strips quarantine (`xattr -dr com.apple.quarantine`, mac/Sources/main.swift:1008), so Gatekeeper is never involved.

## 2. Issue #214 (security report, 2026-10-15). Are the fixes in the pinned commit?

Source: https://github.com/nullmoth/nvidia-macos-driver/issues/214. Maintainer comments claim fixes in 2.0.0 and 2.0.3 (PROJECT CLAIM). I checked each finding at 470e391:

| # | Finding | State at 470e391 | Label |
|---|---|---|---|
| 1 | `os_is_administrator()` always TRUE | Now `kauth_getuid() == 0` (kexts/NVRM/os-xnu.cpp:185). **But the ctl client is still open to unprivileged callers on purpose** (NVRM.cpp:1787-1793: "The ctl client stays open to unprivileged callers on purpose"). There is still no `IOUserClientEntitlements` (kexts/NVRM/Info.plist:50-51) and no `clientHasPrivilege`. Only types >1 are refused (NVRM.cpp:1793). | Partly fixed: DEMONSTRATED |
| 2 | `nvkms_copyin/out` treated bit 63 as a kernel pointer | Now returns -14 (kexts/NVRM/os-nvkms-xnu.cpp:304-313). Selectors 4 and 5 (NVKMS from user space) are retired, `sRetired` (NVRM.cpp:1928-1944). | Fixed: DEMONSTRATED |
| 3 | Physical-memory aliasing in `clientMemoryForType` | System-RAM ranges are now owner-checked by pid (`nv_pm_ram_submd(..., fPid)`, os-xnu2.cpp:531-546). The check is enforced by default but can be turned off with boot-arg `nvrmmapowner=0` (os-xnu2.cpp:585-590). GPU ranges are only bounded to "inside one of this card's bars", **BAR0 register file included** (NVRM.cpp:1885-1898; the comment admits that narrowing it broke CoreDisplay). The owner key is a pid, so pid reuse is a residual risk (HYPOTHESIS). | Partly fixed |
| 4 | `sBindFd` unvalidated, plus client-table DoS | Range is checked (NVRM.cpp:2013-2014). The negative value is still kept deliberately and the stale context is preserved on detach (2019-2023). Per-pid quota is 4096 and 4096 slots are reserved for uid<500 (NVRM.cpp:1803-1824). | Fixed (DoS mitigated): DEMONSTRATED |
| 5 | `sGo` unauthenticated | `kauth_cred_issuser` check (NVRM.cpp:2111) | Fixed: DEMONSTRATED |
| 6 | `sWaitEvent` unbounded parking | Global cap of 256 sleepers, 100 ms max (NVRM.cpp:1966-2003). The event counter is still returned to any caller (minor). | Fixed: DEMONSTRATED |
| 7 | Hard-coded kernel addresses (`kKdk*`) | Under `#if NVRM_PANIC_HUNT` (os-xnu2.cpp:596-601, 724, 796-798). **The shipped NVRM binary contains none of the 7 constants or the 16-byte signature, and does contain the "only in NVRM_PANIC_HUNT=1 builds" stub string** (hashed and scanned in dl-releases). | Fixed in shipped binary: DEMONSTRATED |
| 8 | App self-update trust (codesign without anchor; SHA from same origin) | Ed25519 detached signatures now exist (mac/Sources/ReleaseSignature.swift; p1401/release_sig.py). `codesign --verify --deep` still has no requirement (main.swift:1003) but is now secondary. The main app is still ad-hoc (`-s -`, mac/build.sh:92) with hardened runtime. See §5. | Mitigated: DEMONSTRATED |

## 3. Kernel user-client attack surface (sampled)

The only `IOUserClientClass` personality is `NVRMUserClient` (kexts/NVRM/Info.plist:50-51).

`NVRMUserClient::externalMethod` dispatch table (kexts/NVRM/NVRM.cpp:1921-1937):

| Sel | Handler | Args (scalarIn, structIn, scalarOut, structOut) | Privilege | Notes |
|---|---|---|---|---|
| 0 | `sGo` | 1,0,0,0 | root | OK |
| 1 | `sEsc` | 1, var, 0, var | **any user** | Input size is limited to 1..1 MiB (2034-2035) and copied into a kernel buffer. Output is written back only if the output size is at least the input size (2048-2050). The descriptor path relies on `writeBytes` clamping. `esc()` → `rm_ioctl(NULL, nv, &fNvfp, cmd, data, size)` for every unknown cmd (2097-2100). **The whole RM ioctl surface (NVIDIA's code, written for Linux) is reachable by any local user.** On Linux, `/dev/nvidiactl` is also world-accessible, so this matches upstream exposure, but macOS adds no sandbox or entitlement layer. |
| 2 | `sGetFd` | 0,0,1,0 | any | Returns slot index |
| 3 | `sBindFd` | 1,0,0,0 | any | Range checked |
| 4,5 | `sRetired` | — | — | Unsupported |
| 6 | `sWaitEvent` | 2,0,1,0 | any | Capped |

`clientMemoryForType` (NVRM.cpp:1864-1919): the ctl path uses subtract-not-add bounds (fixed overflow, 1874-1879). The device path maps physical ranges. Any range classified as GPU (`nv_pm_is_gpu`) is mapped with `withPhysicalAddress`, and writable unless RM's `prot` says otherwise. RM, not the kext, decides which ranges a client may put in its mapping context. **HYPOTHESIS:** a bug or missing privilege check in the RM mapping-context path would give a user writable BAR0 access. That means full GPU control, and with VT-d typically disabled on hackintoshes, GPU DMA to arbitrary host memory, i.e. kernel compromise.

NVAccel bridge (kexts/NVRM/accel/nvrm-accel.cpp:619-663): any opener of the accelerator with type `0x4E56xxxx` gets an NVRM client of type `type & 0xFFFF`, created via `callPlatformFunction("nvNewAccelClient")` (NVRM.cpp:1743-1757). IOAccelerator classes are commonly reachable from sandboxed GPU and renderer processes (e.g. browser GPU processes), so **this could expose `rm_ioctl` to sandboxed processes** (HYPOTHESIS; I did not check sandbox profiles).

RW sysctls (`debug.nvaccel*`, `debug.nvrmfb_*`, e.g. nvrm-accel.cpp:109,1419,1431; nvrm-fb.cpp:3571-3617) lack `CTLFLAG_ANYBODY`, so writing them needs root (DEMONSTRATED). Physical-address diagnostics need boot-arg `nvrmdiag=1` and root (os-xnu2.cpp:334-341).

I did not audit the RM ioctl implementations themselves (NVIDIA code). The `esc` buffer handling, the ctl-path size check and the device-path BAR bound look correct as written.

## 4. install.sh privileged actions (package/install.sh @470e391, read in full; 592 lines; runs as root)

Writes:
- `/Library/Extensions/{NVRM,NVAccel,NVRMFB,NVRMAGDC}[,NVPrimarySelector].kext` (350-354)
- `/Library/NullMoth/{kexts,os-major,driver-version,licenses,install.lock,backup-*}` (322-366, 465-468)
- `/Library/GPUBundles/{NVMTLDriver.bundle,NVIDIAShared.bundle,nvmtl,nvmtl-allow.txt}` (391-401)
- firmware at `/Library/NullMoth/nvfw` (402-403); `/Users/Shared/nvfw` is deleted (405)
- `/Library/KernelCollections/AuxiliaryKernelExtensions.kc` via `kmutil create --allow-missing-kdk` (453-458)
- **direct sqlite writes to `/var/db/SystemPolicyConfiguration/KextPolicy`** (414-427)
- NVRAM deletes of `nullmoth-remove`, `nullmoth-nv-starts` and `nullmoth-nv-ok` (473-482)
- LaunchDaemons `com.nullmoth.lastgood` (a root script that can restore the backup and `shutdown -r now`) and `com.nullmoth.bootguard` (483-566)

Good practices observed (DEMONSTRATED):
- fixed PATH (line 4)
- private root-owned copy of the package before checking (176-186)
- refuses symlinks that leave the package (179-181)
- `shasum --strict -c SHA256SUMS` (188)
- root-owned, non-writable parent checks including ACLs (198-219)
- lock directory with stale-owner recovery
- the `/Users/Shared` planted-folder issue is handled (341-345, 372-375, 58, 506)
- shader caches owned by `_windowserver` and not writable by others (439-452)
- SQL inputs regex-validated (418, 423)

Weaknesses:
- **Integrity is self-referential.** `install.sh` checks only the in-package `SHA256SUMS`. The Ed25519 check happens in the 1401 apps before they call it. A manual install from the tarball has no authenticity check unless the user verifies the `.sig` themselves. (DEMONSTRATED)
- `nullmoth-runtime-check.sh` is run from `$SELF`. With `--payload`, `$SELF` is the original script folder, which is not covered by the staged copy check (194-196). This is low risk because install.sh itself comes from the same folder.
- `primary_apply` re-signs kexts ad-hoc after editing Info.plist (258).
- KextPolicy cdhash rewrite: macOS shows no fresh consent for changed kext code at a path the user once approved (414-427). (DEMONSTRATED)
- `ditto "$HERE/Users/Shared/nvfw" "$FW"` copies the firmware; the content check is the kext's SHA-1 table at load (os-xnu2.cpp:1199-1256).

## 5. Update and auto-update trust path (1401)

Windows (p1401/nullmoth.py @f5a9254):
1. `latest_release` / `latest_windows` query `api.github.com/.../releases/latest` (253-286, 366-386).
2. SHA-256 is checked against `SHA256SUMS.txt` from the same release, then `_signed()` downloads `<name>.sig` and runs `release_sig.verify_file` with the compiled-in key `ztPmcj+mcHYM1X2lrp24yjq0xBuzke7gh5XJ3E2fd4E=` (288-306, 317-337, 404-409).
3. Zip-slip is checked (418-421).
4. The new `1401\1401.exe` is returned for launch: it **downloads and executes**.
- The Ed25519 verifier is hand-written in pure Python (release_sig.py:94-131). It does cofactored verification and rejects non-canonical `s >= L`.

Mac (mac/Sources/main.swift):
- The app update checks SHA, then Ed25519 via CryptoKit (998-999), then that `CFBundleShortVersionString` equals the version from the filename (1002), then `codesign --verify --deep` without a requirement. It swaps the bundle via a bash script and **strips quarantine** (1008).
- Driver packages from the release need a signature (1042-1045). A user-chosen local file must match the release SHA and is then also signature-checked (`fromRelease: true`, 933).

Gaps (DEMONSTRATED in code, impact HYPOTHESIS):
- The signature covers file bytes only. The version comes from the asset **filename**. Windows `app_update` and the driver-package path do not check the version inside the artefact, so an origin compromise could serve an *older, validly signed* build under a newer name (downgrade). Exposure begins with the 2.6.6 signatures.
- There is one key and no rotation, revocation or threshold. The public key lives in the same repos the attacker would target. A malicious commit could change both the key and the code, but would then need a release built from it.
- `1401-Mac-2.6.6.dmg` has **no `.sig`**. It is only listed in SHA256SUMS.txt. The DMG is the first-install vector and is not notarised (HYPOTHESIS: there is no Developer ID or Team ID anywhere).
- The mirror (`nullmothsystems.com/mirror`) for third-party kexts is hash-pinned in `p1401/mirror.json` (DEMONSTRATED). NOTICE.md admits two engine-downloaded kexts have no sha256 (AppleIGB, AppleMCEReporterDisabler). That is a PROJECT CLAIM of an open gap.

Signature verification on the v2.6.6 assets: I verified all three `.sig` files independently with OpenSSL 3.5.4 against the published key. All three passed (`nullmoth-nvidia-2.6.6.tar.gz`, `1401-Mac-2.6.6.zip`, `1401-Windows-2.6.6.zip`). DEMONSTRATED.

## 6. Firmware handling (security angle)

- The firmware is bundled in the package at `Users/Shared/nvfw/nvidia/615.78.08/{gsp,ucodes}_{ga10x,tu10x}.bin`. It is not fetched from NVIDIA.
- The kext refuses size 0 or more than 256 MiB, and refuses SHA-1 mismatches for the 4 shipped names (os-xnu2.cpp:1196-1256).
- The SHA-1 values in source match the shipped images exactly (DEMONSTRATED; see provenance doc).
- `gsp_log_*` files have no expected entry, so whatever is at that path is loaded unchecked if present (os-xnu2.cpp:1205-1210, the "nothing is claimed" branch). The directory is root-owned, so the risk is low.
- PROJECT CLAIM (code comment): GSP authenticity is enforced by the GPU's own secure boot.

## 7. Dependency vulnerabilities (best effort)

- NVIDIA bulletin 5861 (published 2026-09-30, 114 CVEs; max CVSS 7.8 per secondary sources). Secondary sources list fixed Linux branches R615 ≥ 615.71.09 and R610 ≥ 610.57.04. Official: https://github.com/NVIDIA/product-security/blob/main/2026/5861/5861.md. Secondary: https://lilting.ch/en/articles/nvidia-gpu-driver-vgpu-security-fixes, https://ubuntu.com/security/CVE-2026-47600.
  - 615.78.08 (released 2026-10-07, after the bulletin) is above 615.71.09, so it is **likely patched** for that bulletin (HYPOTHESIS; I did not read the official mapping).
  - The kernel-mode CVEs in the bulletin apply to NVIDIA's RM code, which this kext contains. The macOS shim may re-introduce classes of bug that upstream fixes assume Linux semantics for.
  - The 610.57.04 components still shipped (`libnvidia-ptxjitcompiler.so.610.57.04`; the 610-based `libnvkms.a` completion patches in kexts/NVRM/patches) sit at the stated fixed floor for R610 (HYPOTHESIS).
- NVIDIA open-gpu-kernel-modules has newer releases (615.71.10 on 2026-10-21, 595.112.07, 580.196.07). 615.78.08 is still the newest 615.78 build.
- Mesa: binary reports `Mesa 26.3.0-devel`, base `17ca6174` (short hash). I could not resolve it on the GitHub mirror (404), and gitlab.freedesktop.org blocked the fetch (Anubis). Built `-Dbuildtype=debugoptimized` (build/build263.sh:11).
- Homebrew LLVM 21.1.8, NVVM 7.0.1 (clang 7.0.1 / 3.8.0 internals), CUDA 13.3 PTX JIT, Vulkan loader 1.4.362, libzstd: all prebuilt and unpinned to any source. I did not assess CVEs for these.
- 1401 Windows: Python 3.13.17 embeddable, pywin32 312, WMI 1.5.1, certifi 2026.10.23, all sha256-pinned in BUILD-MANIFEST.json release_inputs. .NET SDK 8.0.425 (global.json). GitHub Actions pinned by commit SHA (windows-candidate.yml:24,31,51,89).

## 8. CI risk (1401 only; the driver repo has no workflows)

- `windows-candidate.yml` and `hardware-capture-validation.yml` trigger on `workflow_dispatch` and push to `support/*` branches. There is **no `pull_request_target`**.
- Permissions are `contents: read`, `persist-credentials: false`, and actions are pinned by SHA (good, DEMONSTRATED).
- `GH_TOKEN: secrets.GITHUB_TOKEN` is exported to every step (windows-candidate.yml:21), including the step that **runs the freshly built `1401.exe`** (47) and project Python (76-86). With `contents: read` the impact is limited (HYPOTHESIS: low).
- The release was uploaded by user `nullmoth`, not by Actions. The CI uploads only a 3-day artifact, so publishing is manual (see the provenance doc).

## 9. Not done / blockers
- I did not download or compare the CI artifact for run 41263830256 (`windows-candidate`, digest `sha256:425c9f01…`, expires 2026-11-04). It was outside the allowed scope. **Recommended before expiry**: unzip it and compare the inner zip with the release asset `a7648ccb…`.
- No macOS host, so no dynamic testing, no `codesign -dvvv`, no kmutil or sandbox profile checks.
- RM ioctl internals, NVRMFB/NVAccel IOAccel paths and the Metal plugin were not audited line by line.
