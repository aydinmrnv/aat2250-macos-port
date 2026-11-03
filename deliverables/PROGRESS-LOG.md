# macOS on Alienware AAT2250 — progress log

## 2026-11-02

### Completed
- Read prior assessment (untrusted; verified independently where noted).
- Read-only hardware verification → `evidence/hardware-verified.md`. New facts vs prior report:
  - One 4K monitor is currently driven by the Intel iGPU (8086:7D67). macOS has no Arrow Lake iGPU acceleration, so every monitor must be cabled to the RTX 5090 for macOS.
  - The Windows NVMe is a self-encrypting (SED) SK hynix drive behind Intel VMD.
  - The RTX 5090 is Dell-subsystem 1028:5305.
  - Secure Boot is reported on via the registry. BitLocker status is still UNKNOWN because the query needs admin.
- Pinned sources (cloned with `--filter=blob:none`, no submodules, no hooks, no scripts run):
  - nullmoth/nvidia-macos-driver `470e391e8cf8d7e499cdabfa7f2daadb6e14bcfd` (v2.6.6)
  - nullmoth/1401 `f5a9254079601d69a2e165fcd9f659e4f882bab9` (v2.6.6)
  - acidanthera/OpenCorePkg `8d8d690cb543f63114340cdae958de4ce7a8296f` (1.0.8)
- First source finding: the string `2B85` appears in exactly one place in the driver tree, the installer's ID list (`package/install.sh:159`). No kext, test or doc refers to the 5090 specifically.
- Started four isolated agents (NVIDIA / platform+OpenCore / IO / security), each writing only to its own `deliverables/aN-*` directory. Only A1 may edit driver source, and only in worktree `work/nvidia-a1`.

- A3 (IO) done → `deliverables/a3-io/`. Ethernet E3100G: GO (LucyRTL8125 v1.2.4 matches 0x300010ec). BT: conditional GO. Wi-Fi BE200: NO-GO for first boot. ALC1220: Sequoia GO with layout-id search; Tahoe needs AppleHDA root patch. USB: PCH 7F6E has 26 ports, map needs physical plug tests.
  - Correction to the hardware evidence: 15EF/15F0, the Realtek hubs, 0BDA:402E USB audio and 0BDA:8153 USB GbE belong to a Dell WD19TB Thunderbolt dock behind USB4 7EC2. They are not onboard devices.

- A4 (security) done → `deliverables/a4-security/`.
  - The release cannot be traced to source end-to-end. NVRM/NVRMFB have no in-tree build recipe: `rmcc.py:6` refers to a `build-nvrm.sh` that does not exist in the repo or its history (spot-checked). There are no CI workflows (spot-checked: `.github` holds only templates). The Darwin port of NVIDIA's open kernel modules and several prebuilt toolchain pieces are not published.
  - The asset hashes match GitHub's digests and SHA256SUMS, and the Ed25519 .sig files verify. GSP firmware is bundled and matches the SHA-1 table in `os-xnu2.cpp`.
  - Top risks: SIP 0x0A43 plus AMFI off plus SecureBootModel=Disabled (spot-checked in `README.md:51-53`); `rm_ioctl` reachable by any local user, with BAR mapping; unsigned kexts; the installer writes the KextPolicy database.
  - Pending user decisions: compare the CI artifact (expires 2026-11-04) against the release zip; fetch NVIDIA's 615.78.08 .run to confirm the bundled firmware is unmodified.

- A2 (platform) done → `deliverables/a2-platform/`.
  - All three internal disks are behind VMD AD0B. No standalone AHCI or NVMe controller is visible. Today the only viable target is a blank external USB SSD; whether M.2 slots 2–3 are mapped by VMD needs a look in the BIOS.
  - CPU risk is HIGH: the Comet Lake CPUID spoof is needed and the Arrow Lake evidence is third-party only. Tahoe on Arrow Lake is unproven.
  - Confirmed with `powercfg /a`: S0 Low Power Idle only, no S3. Sleep/wake is expected to fail until shown otherwise.
  - Firmware Secure Boot will in practice have to be off to boot macOS with the driver's SecureBootModel=Disabled. This is the user's decision.
  - OpenCore draft: `#WARNING`=DRAFT - NOT BOOT READY, with 23 UNRESOLVED values. ocvalidate 1.0.8 (official zip, hash checked) reports 1 expected error (placeholder SystemUUID).
  - Checked: the ACPI copies (DSDT/FACP/APIC/DMAR) are scratchpad-only, and no MSDM (Windows key) table was dumped.

- A1 (NVIDIA) done → `deliverables/a1-nvidia/`.
  - No public GB202 test exists; 1401's tested-ID list is only 2D05 (RTX 5060).
  - GB202 shares firmware (gsp_ga10x) and FSP code with GB206/GB207. Its 3D, compute, copy and display classes are identical; only the video engine counts differ (4/4 vs 1/1).
  - Biggest 5090-specific risk: BAR1 = 32768 MiB per `nvidia-smi`, while the project has only proven an 8 GB BAR1.
- Patch: NVRM GET_ENGINES_V2 video-engine instance diagnostic (aimed at #425), branch `port/a1-5090`: 7f99e4b followed by 9c76447 (review fixes).
  - Windows-side validation: `python -I tools/test_gpu_profile_video_engines.py` PASS against the 615.78.08 tree and the 610 headers; mutation controls FAIL as intended; `test_copy_class_selection.py` PASS.
  - Independent review: approve-with-changes; changes applied.
  - **Not compiled; not run on macOS or on a GB202.**

### Remaining blockers (none are resolvable from Windows)
1. **Hardware:** a blank external USB SSD is needed (all internal disks are behind VMD).
2. **Admin checks by the user:** `manage-bde -status` for BitLocker/SED state, and confirming the recovery key is accessible.
3. **BIOS (look only, no changes):** VMD mapping for M.2 slots 2–3, any S3 option, Secure Boot key management.
4. **Decisions:** the Secure Boot strategy, and acceptance of the SIP/AMFI reduction the NVIDIA driver requires.
5. **Approvals for tools:** an ACPI dump (excluding MSDM) and running USBToolBox on Windows for port mapping.
6. **macOS build host:** Mac Studio M1 Max, macOS 27.0, Xcode 27.0. It is reachable by SSH key, and an x86_64 kext object compile is DEMONSTRATED. Blockers: only 62 GiB free (the VM plan needs about 150 GB), and the toolchain (brew, rust, meson, iasl) is missing; installing it needs approval. Original note: the user has a Mac Studio (Apple Silicon). Plan in `MAC-STUDIO-BUILD-HOST.md`. It still needs the chip and OS details, an access method, and approval for a build VM. The NVRM core build script is still missing upstream and has to come from the maintainer.
10. **Target changed to macOS Tahoe (user decision, 2026-11-02).** Consequences: AppleHDA is gone (audio via restore, VoodooHDA or USB); the BE200 alpha is Tahoe-only; Tahoe on Arrow Lake is unproven; Tahoe is the last Intel macOS. The A2 config draft and plan were written Sequoia-first and need a Tahoe pass.
11. **SSD D decision pending:** USB enclosure (recommended) or a custom VMD driver. Either way the data on the disk must be backed up first.
7. **Physical tests on this GPU:** see `a1-nvidia/macos-gpu-tests.md` and `a3-io/io-acceptance-tests.md`.
8. **Optional (A4, user approval needed):** compare the CI artifact (expires 2026-11-04) with the release zip; fetch NVIDIA's 615.78.08 .run to confirm the bundled firmware is unmodified.
9. **Cabling:** connect all monitors to the RTX 5090. The WD19TB dock/iGPU display path is not supported for macOS bring-up.

## 2026-11-02 (later): Mac Studio work
- **Toolchain:** installed iasl 20260408, meson 1.12.1, ninja 1.13.2 and Rust 1.98.1 (x86_64 target) as the user, with no sudo. The default python3 was briefly re-pointed to 3.15 by Homebrew; 3.14.7 is restored.
- **Disk space:** moved 30 GB of Xcode caches to the Trash. The user emptied it, and the Mac now has 101 GiB free.
- **Patch compile:** the 5090 patch compiles for x86_64 against the macOS 27 SDK, with no new warnings versus baseline. Not linked and not run.
- **SSDT-PLUG-ALT:** built for 24 cores from this machine's DSDT/MADT values → `acpi/`.
- **New risk:** the MADT is x2APIC-only. A first-boot test has been added.
- ACPI: full dump done (no MSDM). Built SSDT-EC-USBX (needs an ECDV _STA rename patch). SSDT-AWAC is blocked by a \_INI collision. SSDT-PMC is unresolved (dynamic PWRM base). See acpi/README.md.
- ACPI follow-up:
  - The AWAC "\_INI collision" was a grep false positive; there is no root \_INI, so SSDT-AWAC is usable.
  - All three SSDTs are compiled, hashed and copied back to `acpi/`.
  - SSDT-PMC moved to an evidence-based first-boot NVRAM persistence test.
- OpenCore draft updated for **Tahoe 26.7.2**:
  - SSDTs referenced by hash; targeted ECDV `_STA`→`XSTA` patch added.
  - HDA path confirmed; Tahoe audio and Bluetooth caveats recorded.
  - ocvalidate: only the expected placeholder error. Still NOT boot ready (22 UNRESOLVED).
- Decisions delegated by the user, recorded in `DECISIONS.md`: Tahoe audio phase 1 = USB audio (AppleALC disabled); SSD D route = USB enclosure (backup first, user action).
- Pinned 11 release packages by SHA-256: 9 from GitHub digests, 2 hashed on the Mac. The IntelBluetooth 2.4.0 release plist matches 8087:0036. UNRESOLVED count in the draft: 22 → 12. ocvalidate: still only the expected placeholder error.

## 2026-11-02 (evening): VMD driver work (user chose "build a VMD driver"; user away, told me not to ask)
**Research (pinned)**
- Sources: Linux vmd.c @2c88c2dc (v7.4-rc1, GPL, read only), FreeBSD vmd.c @57563954 (BSD-2), Apple IOPCIFamily-726.100.6 (= macOS 26.5 per distribution-macOS macos-265).
- **0xAD0B cannot bypass MSI remapping** (both upstreams agree), and it has the ARL004 interrupt-ordering erratum, which needs a dummy read of the child before handling.

**Windows facts**
- VMD BARs: CFGBAR 0xB820000000 (256 MB), MEMBAR1 0x72000000 (16 MB), MEMBAR2 0xB839000000 (16 MB); 32 MSI-X.
- 09AB placeholders sit at PC02.SAT0 (SATA) and PC00.RP21 (NVMe root port).

**Built on the Mac Studio, not loaded or run**
- `VMDProbe.kext`: phase 0, read-only.
- `VMDBridge.kext`: phase 1, an IOPCIHostBridge subclass on a non-PCI nub. AHCI is the only thing it exposes. NVMe and bridges are hidden, MSI is hidden, and the class is disguised at level 1. It has an ABI size check and boot-arg kill switches.
- Copies are in `vmd-driver/dist/`. Design, test ladder and risks are in `vmd-driver/DESIGN.md` and `README.md`.

**Blocked**
- Building IntelBluetoothFirmware/Lilu from source was refused by the permission system ("code from external"), so it needs the user's OK.
- The x86_64 IOPCIFamily symbol and size check against Tahoe 26.7.2 needs the KDK or the installer's kernel collection.

**Non-destructive copy:** the user's plain folders on D: were copied to another internal disk with robocopy (no deletes). All copies verified complete with 0 failures; D: is unchanged. Folder list and logs are kept privately. App-managed libraries (Store/Steam), a WSL distro and the pagefile are left for the user to move with their own tools.
- VMD phase 2 (interrupt routing, `vmdbridge=2`) written and building (`VMDInterrupts.cpp`). Not yet reviewed or run.
- **VMD driver reviewed twice and fixed** (`vmd-driver/REVIEW-phase0-1.md`, `REVIEW-phase2.md`).
  - Phase 1: only the ABAR range is advertised, CFGBAR length is checked, the PM tree is joined, MSI length is read from control.
  - Phase 2: pending replay, wait for in-flight handlers, MSI re-programmed on restore, VMD vector count checked, extra memory BAR refused, mask bits cleared.
  - VMDBridge binary sha256 `7a78d810…`. Builds with -Werror. **Never loaded or run.**
- **Installer-test EFI assembled** on the Mac Studio (`~/macos-port-build/efi-stage/EFI-installer-test`).
  - Contents: OpenCore 1.0.8 + 11 kexts, each zip SHA-256-verified; OpenHfsPlus.efi; the 3 SSDTs; VMDProbe + VMDBridge.
  - SMBIOS generated locally with macserial, never displayed; config kept in `private/`.
  - ocvalidate: no issues.
  - Runbook: `FIRST-BOOT-RUNBOOK.md`. The first boot is installer-only, with VMDProbe reading the VMD domain and nothing written to internal disks.
- Tahoe 26.7.2 full installer download started on the Mac Studio via `softwareupdate`.
- The USB stick is not yet connected to the Mac Studio.
- **Recovery test USB built and verified.**
  - The user's 3.8 GB stick (too small for the full installer) was erased with the user's explicit approval and the UAC prompt, after the disk identity was checked in-script, then formatted FAT32 "OPENCORE".
  - Contents: Tahoe recovery (macrecovery.py → osrecovery.apple.com; BaseSystem.dmg 916 MB, chunklist-verified) + installer-test EFI (ScanPolicy 0x200703 so FAT32 recovery is listed).
  - All 39 files hash-match. Ready for the first boot test (`FIRST-BOOT-RUNBOOK.md`).
- Full Tahoe 26.7.2 installer now on the Mac Studio (/Applications/Install macOS Tahoe.app, app version 21.7.12). It is kept for a future 16 GB+ USB; not needed for the recovery-stick test.

## 2026-11-03: Dock and iGPU made requirements
- The user requires the WD19TB dock and the iGPU monitors to work under macOS.
- Wiring evidence (read-only, Windows):
  - Both LG Ultra HD monitors run on iGPU 7D67 over DP. The S3225QS is on the 5090.
  - The AAT2250 has no motherboard video outputs, so the iGPU signals leave through the 2 rear TB4 ports.
  - Windows uses the Microsoft USB4 software CM.
- Prior art: XeGfx 0.0.46 (commit ff64f836) is cloned for review on the Mac Studio and has not been built. It claims Metal on 7D67 laptops (eDP only). External displays are not implemented (`docs/xg_misc.md`, X6). No macOS USB4 CM for Intel exists.
- Plan written: `DOCK-AND-IGPU-PLAN.md`.
  - Phases: D0 observations at the test boot; D1 USB4Probe (read-only); D2 PCIe tunnel; D3 DP tunnel; G1 Type-C modeset on XeGfx; G2 dual-GPU desktop; D4 hot-plug.
- Runbook: added the `ioreg-full.txt` capture and monitor observations to the test boot.

## 2026-11-03: Phase D1 USB4Probe built and added to the recovery stick
- `usb4-driver/USB4Probe`: a read-only kext for NHI 8086:7EC2 (Linux calls it MTL_P_NHI0), gated by `-usb4probe`.
  - Reads PCI state and a 16-register MMIO allowlist. It never reads the clear-on-read notify registers.
  - Reads TBT root ports 7EC4–7EC7 (link and slot state) and lists the devices enumerated behind them.
- The key question it answers: is Intel's firmware connection manager (ICM) running?
  - Linux supports ICM on this NHI ID when the OS has not taken USB4 control through _OSC, and macOS never takes it.
  - If ICM is running, phase D2 shrinks from a full software CM to an ICM client.
- Build is clean. sha256 `ad72ef6b…98bb`. Self-review notes are in `usb4-driver/README.md`.
- Private config: added the USB4Probe entry and `-usb4probe`; ocvalidate reports no issues. Kext and config are copied to the stick (F:) and hash-verified.
- Runbook: added the `usb4probe.txt` capture and a pass row.

## 2026-11-03: CUDA step C0 (static analysis), see `cuda/C0-REPORT.md`
- Downloaded NVIDIA Linux driver 615.78.08 from download.nvidia.com; its SHA-256 matches NVIDIA's published sum. Extracted it without running the installer.
- libcuda findings: it links only glibc (248 imports), has no ELF TLS, is x86_64, and is the same RM version as the macOS NVRM kext.
  - All RM escapes it uses are reachable through nullmoth's generic `rm_ioctl` path.
  - Gaps: 7 Linux-layer escapes (small) and UVM (absent, large).
- **Licence issue:** NVIDIA LICENSE §2.2 forbids disassembly. The ioctl scan used `objdump -d`, which breached that. The disassembly was deleted, and the report relies only on ELF metadata and open-source definitions. No further disassembly will be done.
- Verdict: no static blocker. All runtime gates wait for the 5090 bring-up.

## 2026-11-03: CUDA step C3 part 1, `cuda-shim` (ELF loader + glibc shim)
- `cuda-shim/`: an ELF loader and a glibc shim covering all 248 libcuda imports. Process creation, sockets and eventfd are refused and logged. Build is clean.
- Run on the Mac Studio under Rosetta with the unmodified libcuda 615.78.08:
  - **L1 PASS:** loads, relocates and the constructor runs.
  - **L2 PASS:** `cuGetErrorString` gives NVIDIA's text; `cuDriverGetVersion` returns 13040.
  - **L3 PASS:** `cuInit` returns cleanly.
- Open: `cuInit` returns 304 OPERATING_SYSTEM before any device access. Right after it sets up process-shared locks, no shim call fails, so the cause is most likely a direct Linux syscall.
  - Proposed next step: a reference `strace` of cuInit on Linux in an ephemeral Docker container. Needs approval.
- Tools added: `CUDASHIM_TRACE=2` gives an ltrace-style call log at the interface (no disassembly). The `getenv` and formatted-string traces surfaced libcuda's own log line.

## 2026-11-03: CUDA L3 matched to Linux
- Ran a reference `strace` of `cuInit` on Linux: a throwaway ubuntu:24.04 container, no GPU. It returns 100 NO_DEVICE. Trace kept in `evidence/cuda/`.
- Cause of the macOS 304: libcuda `dlsym`s `pipe2`, `accept4`, `sched_getcpu` and the affinity functions; the shim returned NULL, so its eventfd and worker-thread setup failed.
- Added those functions, eventfd emulation (socketpair) and Linux `/proc` readlink semantics.
- `cuInit` now returns **100 CUDA_ERROR_NO_DEVICE** on the Mac Studio, following the same path as on Linux. Stable over 3 runs.
- Docker Desktop side issue:
  - Launching it from the Claude app put its AF_UNIX sockets in the app's virtualized AppData, and Windows couldn't open them afterwards. The 24 Oct `run.stale-*` folders show the same thing happened before.
  - Fix: the broken folders were renamed `*.stale-20261103b` (nothing deleted), and the user starts Docker from the Start menu.
  - Leftover: the ubuntu:24.04 image (~78 MB) is still in Docker.

## 2026-11-03: First test boot. OpenCore halted with "System has no boot entries"
- Result: OpenCore started (Secure Boot off works), then `OCB: System has no boot entries` / `OC: Failed to show boot menu! Halting on critical error`. Windows booted normally afterwards.
- Cause (config error, mine): ScanPolicy `0x200703` set `OC_SCAN_FILE_SYSTEM_LOCK` and allowed only APFS, HFS and ESP. The stick is an MBR FAT32 partition (type 0x0C, not an EFI System partition), so it was filtered out, and with it the recovery dmg.
- Fix: ScanPolicy `0x200002` (device lock, USB only, any filesystem). Internal disks are still never scanned. ocvalidate reports no issues. The stick's config is updated and hash-verified; the public `efi/config.plist` is updated too.
- Side effect reported: Windows said the PIN was unavailable afterwards. This is the expected Windows Hello behaviour after a Secure Boot (TPM measurement) change; OpenCore wrote nothing to Windows. Fix: sign in with the password, then reset the PIN, or turn Secure Boot back on.
- **Second attempt:** the picker showed `1. OpenCore (external)` and `2. OpenCore (external) (dmg)`. The dmg entry is the Tahoe recovery. Keys did not work in the picker, so the timeout defaulted to entry 1, the stick's own `EFI/BOOT` launcher.
  - Fix: `EFI/BOOT/.contentVisibility` = `Disabled`, a documented OpenCore 1.0.8 mechanism (Configuration.tex §boot algorithm). The recovery dmg is now the only entry and boots at the 10 s timeout without a key press.
  - Added to the stick, to the Mac's efi-stage and to `tools/build-efi.sh`.
  - Keyboard: KeySupport is already on. Next boot, plug the keyboard into a rear USB port; the dock's USB may not be up before OpenCore.
- **Pre-emptive config fixes before the third attempt.**
  - Reference: 7HE-W0R1D/Hackintosh-Z890M `EFI-Tahoe` (Core Ultra 265K, Tahoe, MacPro7,1, pushed 2026-10-31). Treated as untrusted evidence; settings were compared, nothing was copied wholesale.
  - Adopted:
    - `ProvideCurrentCpuInfo=true` (real CPU and TSC frequencies alongside the Comet Lake CPUID spoof);
    - `ProtectUefiServices=true` (recommended for Z390-and-newer firmware);
    - `PollAppleHotKeys=false` (documented to make keyboard input unreliable on some firmware; a suspect for the dead picker keyboard);
    - `ReleaseUsbOwnership=false` ("not recommended unless required").
  - Kept `AppleXcpmCfgLock=true`, because Dell firmware can't unlock CFG Lock.
  - Not taken: the reference's DSDT-specific IRQ and `_OSI` patches (that board's own firmware), and SSDT-PMC (NVRAM; not needed for a recovery boot).
  - Logging: `DisplayLevel` 0x80000002 → 0x80000042, so the OpenCore log on the stick includes INFO lines. The first two logs held only the error lines and confirmed the diagnoses: "no boot entries", then entry 1 = OpenCore re-launch ("Already started").
  - ocvalidate: no issues. The stick's config is updated and hash-verified (107ec9ff…).
- **Third attempt (13:13): got further than it looked.** Only the dmg entry was shown, and it was auto-selected at the timeout.
  - The log shows **Apple's boot.efi (bootbase.efi 583~5906) running from the recovery dmg**. It entered recovery boot mode (`BM:ROS`) and read the boot-args (with a `slide=177` added by OpenCore).
  - OpenCore took 78 s before boot.efi (10 s countdown, then loading and verifying the 960 MB dmg from a USB 2.0 stick). boot.efi logged about 17 s more, and the log ends where it loads the kernel from the dmg, with no error line.
  - Most likely it was still loading when it was switched off. No failure was recorded.
  - Changes:
    - `ShowPicker=false`: the single entry boots immediately; no menu, no keyboard needed.
    - `-lilubetaall` added, so Lilu plugins can't refuse to load on 26.7. The reference Tahoe build uses it; Lilu 1.7.2 itself supports 26.
  - ocvalidate: no issues. Stick config hash 27b1897d….
- **Fourth attempt (13:2x): the kernel handoff hang.** No picker; boot.efi ran fully. It loaded `BootKernelExtensions.kc` from the dmg (Ok), logged `CSR:OUT 0xA43`, then `EB|LOG:EXITBS:START 13:23:02`. Nothing after that: no kernel verbose output. (`OS.dmg.root_hash` was not found; boot.efi continued past it.)
  - Diagnosis: the Dell MADT has **zero type-0 Local APIC entries**, only x2APIC ones (re-parsed from `evidence/acpi/APIC.aml`). XNU finds no CPUs and stops before the console.
  - Independent matches: an Arrow Lake laptop with the same EXITBS hang (WTLIntelGfx issue #11), and the Legion 5 15IAX10 OpenCore repo (`APIC-legacy.aml`).
  - Fix: our own `deliverables/acpi/x2apic_to_lapic.py` produces `APIC-LAPIC.aml` (24 LAPIC entries). OpenCore deletes the firmware APIC (length 856) and adds the converted one.
  - ocvalidate: no issues. Stick updated and hash-verified: config 4aba9c0b…, table d5496ada….
  - This had been flagged as a risk ("MADT x2APIC-only") since the first analysis, and it was not fixed before the first boot. That is my miss.
