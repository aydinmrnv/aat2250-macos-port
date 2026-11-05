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
- **Fifth attempt (13:43): the MADT fix worked. The macOS kernel is running.** boot.efi hit EXITBS at 13:45:19 (log on the stick), and kernel verbose output followed.
  - Photos show AppleACPIPlatform up, ACPI `_DSM` evaluations on `\_SB.PC00.TXHC.RHUB` and `SS01–SS04` failing with `AE_NOT_FOUND`, then `VirtualSMC efend: authenticated restart support is unavailable`, and then no further output at the time of the photo.
  - The TXHC `_DSM` errors come from the Dell TCSS USB-C ACPI code under macOS's ACPICA 20160930. Our only ACPI patch is confirmed scoped to `\_SB.PC00.LPCB.ECDV._STA` (DSDT, count 1), so it is not the cause. They are expected to be non-fatal.
  - VirtualSMC's efend message is normal. Open question: how long it sat on that line. If it's stuck, XNU prints either "Still waiting for root device" or IOKit busy-timeout lists after about 1–5 minutes; those identify the blocker.
  - User estimate: about 5 minutes on that line with no further output. No "Still waiting for root device" and no busy-service list, so a silent hard hang is likely.
  - Prime suspect: our own probes. They start at PCI driver matching, right after VirtualSMC init, and neither probe's first IOLog line appeared. USB4Probe's MMIO reads of a TCSS NHI the firmware may have power-gated can stall the CPU silently; VMDProbe is less likely.
  - Isolation for the sixth attempt: `-vmdprobe` and `-usb4probe` removed from boot-args (the kexts stay installed but inert). Nothing else changed. `boot-args` is in NVRAM Delete, so the new value applies.
  - ocvalidate: no issues. Stick config c54a2c39….
- **Sixth-attempt config: minimal, proven kext set.**
  - Audit finding: **CpuTopologyRebuild and SMCProcessor were disabled.** The booting Arrow Lake Tahoe reference runs CpuTopologyRebuild. The 285K is hybrid (8P+16E, no HT), and the freeze came where CPU power management starts, so this is now a stronger suspect than the probes. Both enabled.
  - Added `kexts/MCEReporterDisabler.kext`, our own codeless plist. It claims AppleIntelMCEReporter's match categories for board-id Mac-27AD2F918AE68F61 (MacPro7,1), the same technique as AppleMCEReporterDisabler, which the reference uses.
  - Disabled entirely: VMDProbe, USB4Probe, VMDBridge. No experimental code on this boot.
  - Enabled set: MCEReporterDisabler, Lilu, VirtualSMC, SMCProcessor, RestrictEvents, CpuTopologyRebuild, LucyRTL8125Ethernet, USBToolBox, UTBDefault.
  - Recovery path check:
    - Keyboard: plugged into a rear PCH USB port, a USB2 device enumerates on HS01–HS14, inside macOS's 15-port limit with UTBDefault.
    - Display: GOP framebuffer, no GPU driver needed for Recovery.
    - Root: the dmg is booted from RAM (MemMap device), so USB storage isn't needed for root.
  - ocvalidate: no issues. All enabled kexts present on the stick. Config a8f0bb4a….
- **Sixth attempt: froze again, at the same point.** The user saw the TXHC `_DSM` errors as the last output, this time without the VirtualSMC line. VirtualSMC's message order varies; the constant is the TCSS xHCI 8086:7EC0 (`\_SB.PC00.TXHC`) root-hub `_DSM` evaluation right before the freeze. This controller was already open as UNRESOLVED-TCSS. The WD19TB dock sits on the same TCSS block.
  - Fix: `kexts/TCSSXHCIDisabler.kext`, codeless. An IOService personality on `IOPCIMatch 0x7ec08086` with score 30000 claims the device before AppleUSBXHCIPCI. Nothing is written to the device.
  - The keyboard (19F5:2620, HS04) and Logitech receiver (HS05) are on the PCH xHCI 7F6E, so they're unaffected.
  - User step for this boot: unplug the WD19TB dock and any USB-C devices; keyboard and mouse go in rear USB-A ports.
  - ocvalidate: no issues. Stick config b97c85e6…, kext plist 86acef13….
- **Seventh attempt: still frozen at the USB `_DSM` errors, with or without the dock.** Root cause found by research.
  - Source: the Lenovo Legion 5 15IAX10 (Arrow Lake-HX, Tahoe 26.7.2 daily; watchthelight-labs/WTLLegionOpenCore, docs/HOW-IT-WORKS.md). It reports "a silent hang after a `\_SB.UBTC._CRS AE_NOT_FOUND` error", caused by Intel Arrow Lake reference ACPI code that macOS's ACPICA 20160930 mishandles.
  - The Dell DSDT has the same constructs. Our own `deliverables/acpi/dell_dsdt_patches.py` makes 7 OpenCore patches, each found exactly once in the Dell DSDT and length-preserving:
    1. `If (PCHA != Zero) { Device (PC02) }` → `Scope (\_SB)`.
    2. The root `If (PCHA != Zero) { MNVB … Scope (_SB.PC02) }` → `Scope (\)`.
    3. `PC00.SBSC`: `Name SBRG` + `SBRG = \SBRG` → a Method.
    4. `PC02.SBPC`: `Name SBRG` + `SBRG = ^SBRG` → a Method.
    5. `PC00.XHCI` (PPID 0x3A): the `DUWS`/`PU2C`/`PU3C` Name+Store pairs → Methods.
    6. `PC02.XHCI` (PPID 0x4F): the same → Methods.
    7. `\_SB.PGPI` (PCH GPIO, INTC1084): `SBRG` and `GEI0-2`/`GED0-2` Name+Store pairs → Methods. **This one is not in the Legion set**; it was found by scanning the Dell DSDT for every Name + bare Store pair.
  - Checked: nothing in the DSDT or SSDTs writes to the converted names. Dell's USB SSDT (`xh_Dell_`) marks HS04/HS05 (keyboard, receiver) connectable; only SS03 is `GUPC (Zero)`, which is probably correct.
  - Also from the Legion build:
    - CpuTscSync 1.1.2 (sha256 bc289f78…, matches GitHub's digest). Their first kernel boot panicked with "Non-monotonic time" in `AppleACPICPU::start`.
    - `ProcessorType 0x0F01` (24 cores with the Comet Lake spoof).
    - `ReleaseUsbOwnership=true`.
  - Not verified: loading the patched DSDT in ACPICA 20160930 acpiexec. Building ACPICA from source on the Mac was blocked by the permission classifier; it needs the user's OK.
  - ocvalidate: no issues. Stick config eb462084….
- **Eighth attempt: the ACPI fix worked.** No `AE_NOT_FOUND` errors. Both PCI domains were configured: one pass "bridges 7, devices 16", then PC00 "bridges 10, devices 26". `Console relocated to 0xa018000000`, then "Previous shutdown cause: 5", then "SMOS has arrived".
  - **Dock finding:** `pci8086,15f0` (the WD19TB's Titan Ridge xHCI) was enumerated by macOS. The Dell firmware leaves the dock's PCIe tunnel up at handoff, which answers the D0 question: dock USB, Ethernet and audio may work through Apple's own drivers without our connection manager. Hot-plug still needs one.
  - **Then it stopped for 4 hours.** The last output was USBToolBox polling controllers (TXHC, which our blocker was holding; TDM0, the NHI rather than a USB controller; XHCI; 15f0): "waitForMatchingService failed or timed out", fallbacks, "Installed controller notifier". Nothing after that.
  - Change for the ninth attempt: USBToolBox, UTBDefault and the TXHC blocker are disabled. USB is left to Apple's drivers, and the TCSS USB-C ports and the dock's xHCI are included again. The Legion Arrow Lake build boots without USBToolBox. The earlier TXHC errors were a symptom of the DSDT rejection, now fixed.
  - Enabled set: MCEReporterDisabler, Lilu, CpuTscSync, VirtualSMC, SMCProcessor, RestrictEvents, CpuTopologyRebuild, LucyRTL8125Ethernet. ocvalidate: no issues. Stick config 921b8689….
- **Ninth attempt: same freeze point, with more of the log visible.**
  - All 24 CPUs registered (`AppleACPICPU: ProcessorId=… LocalApicId=… Enabled`), confirming the MADT fix. VirtualSMC started, then IOAPIC, then "Executed 137 blocks of module-level executable AML code", sleep states S4 S5, both PCI configuration passes. Frozen after "PCI configuration end, bridges 10, devices 26", the start of driver matching.
  - **`SsdtEC` failed to load all along:** `[_STA] … AE_ALREADY_EXISTS` and "1 table load failures, 27 successful". The `ECDV._STA → XSTA` patch used an ACPI-path `Base` that OpenCore did not resolve, so our SSDT's `_STA` collided. The result: no fake EC, no USBX, and Dell's real EC (PNP0C09) exposed to AppleACPIEC, a known desktop hang source.
    - Fix: a base-less 12-byte unique match `14095F53544108A40A0F144A` → `…58535441…`, at DSDT 0x8BE81.
  - **`Unsupported module-level executable opcode 0x70` warnings:** found with `deliverables/acpi/find_module_level_stores.py`, our own scanner. They come from `DellRtd3`, whose Scope-level stores fill the RTD3 GPIO packages (`PWRG`/`RSTG`/`WAKG`) for PC00.RP01, PC02.RP05 and RP09. macOS skips them, so the RTD3 `_ON`/`_OFF` methods would drive GPIO pad 0.
    - Fix: drop `DellRtd3` (OEM ID DellRtd3, length 56991) for macOS only. Checked that nothing outside it depends on its objects.
  - Still open: one `[_SB_.PC02] AE_NOT_FOUND` during deferred module-level code (`\_GPE` return-value warnings follow). It may come from DellRtd3's deferred blocks; to be seen on the next boot.
  - All 8 DSDT patches apply in sequence with unique matches; DSDT size unchanged. ocvalidate: no issues. Stick config cf8b9116….

## 2026-11-03/04: **macOS Tahoe Recovery boots on the AAT2250**
- Tenth attempt (OpenCore log 20:19): the boot reached the macOS Tahoe Recovery UI. The user typed commands in Terminal, so the USB keyboard works with Apple's USB drivers.
- Config that booted (stick config cf8b9116…):
  - **ACPI:**
    - MADT replaced (x2APIC → Local APIC).
    - 7 DSDT patches for Intel Arrow Lake reference code (PCHA module-level If → Scope; SBRG/PU2C/PU3C/DUWS/PGPI Name+Store → Methods).
    - ECDV `_STA→XSTA` by unique byte match, so SsdtEC (fake EC + USBX) now loads.
    - DellRtd3 and DMAR dropped.
  - **Kexts:** MCEReporterDisabler, Lilu, CpuTscSync, VirtualSMC, SMCProcessor, RestrictEvents, CpuTopologyRebuild, LucyRTL8125Ethernet. No USBToolBox, no project probes.
  - **Quirks:** ProvideCurrentCpuInfo, ProtectUefiServices, AppleXcpmCfgLock, ProcessorType 0x0F01, Comet Lake CPUID spoof.
- Result files: not on the stick. macOS mounted it (`.fseventsd` written), but `vmdprobe.txt`, `usb4probe.txt` and `ioreg-full.txt` are absent. Most likely the machine was powered off without Shut Down, so FAT writes were lost. The probes were disabled on this boot in any case.

## 2026-11-04: Emptying D: for the macOS install
- Boot disk confirmed from the Windows partition flags: disk 0 (NVMe) partition 1 is the System ESP, and C: is the boot volume. The Samsung 860 EVO (disk 2: Recovery, ESP, MSR, D:, Recovery) is used by neither. To be re-confirmed right before any erase.
- Inventory of D: (464 GB, 149 GB free): the four plain folders, an empty `Docker` folder, the Store/Xbox install (a Store game, 148.9 GB, plus WindowsApps/WpSystem/Program Files/.GamingRoot), SteamLibrary (12.6 GB), `WSL\NVIDIA-Workbench` (9.5 GB) and `pagefile.sys` (43 GB).
- Done by Claude:
  - Robocopy re-sync of the four plain folders to `E:\copy folder`: 0 changed, 0 failed (about 101 GB, identical). Logs in `evidence/d-to-e-copy/resync-*.log`.
  - `wsl --manage NVIDIA-Workbench --move E:\WSL2-Distros\NVIDIA-Workbench`: exit 0, the distro starts, nothing left on D:.
- Left for the user (app-managed or admin): the Store game via Settings → Apps → Move; Steam via Storage → Move; pagefile from D: to C: in `sysdm.cpl`, then reboot.
- After macOS takes the disk: map drive letter D: to `E:\copy folder` in Windows so old `D:\…` paths keep working.
- **D: emptied (2026-11-03 21:15):**
  - a Store game moved to `E:\Store game folder` with Windows' own Move: 148.9 GB on E:, 0 left on D:.
  - Steam library copied to `E:\SteamLibrary`: 13,059 files and 13,481,045,658 bytes, identical. Steam was signed out, so `libraryfolders.vdf` was repointed from `D:\SteamLibrary` to `E:\SteamLibrary`, with a backup `libraryfolders.vdf.bak-20261103`.
  - The user set the pagefile to C: only (system managed); it takes effect after a restart.
  - Left on D: only data with verified copies on E: (the four plain folders and the old SteamLibrary copy) plus the pagefile until the restart.
- After the restart (21:27): pagefile only on C:, `D:\pagefile.sys` gone, the Store game on E:, Steam pointing at E:, WSL fine.
- Next boot: VMDProbe re-enabled (read-only, `-vmdprobe`, sha256 bd6b4c42… unchanged), USB4Probe still off, so a hang can be pinned to VMDProbe alone. ocvalidate: no issues. Stick config 6134fa28….
- **Prep for daily driving, while waiting for the VMDProbe boot (stick unchanged):**
  - **ACPI verified offline:** iasl and acpiexec 20260408 found on the Mac (`/opt/homebrew/bin`). The patched DSDT (9 patches: the earlier 8 plus `_PTS→ZPTS`), the 23 Dell SSDTs macOS sees (no DellRtd3) and our 4 SSDTs make **28 tables, all loaded, 0 load errors**.
    - The patched objects evaluate: PU2C, SBRG ×3, PGPI.GEI0, ECDV._STA (0x0F for non-Darwin), EC._STA, USBX._STA, XHCI.XPME.
    - acpiexec is ACPICA 20260408, not macOS's 20160930: a load check, not a substitute for a boot.
  - **New:** `SSDT-FIXSHUTDOWN-AAT2250` (the Dell PCH xHCI has no PMEE field, so the table maps PMCSR bit 8 at config 0x75 itself; TXHC uses TcssSsdt's PMEE), plus the `_PTS→ZPTS` patch (unique). Staged in the configs and the Mac's efi-stage; ocvalidate: no issues.
  - **Private config:** `ROM` = the onboard Killer E3100G MAC, for iServices. Not in the public repo.
  - **New:** `tools/make-postinstall-config.py`, the RTX 5090 driver profile (AMFI args, nvfb args, IONDRVSupport blocked, DisableIoMapperMapping, AMFIPass). Output validated. AMFIPass is still to be reviewed and pinned.
  - **New:** `DAILY-DRIVER.md`, the roadmap.

## 2026-11-04: Eleventh boot (VMDProbe on, shutdown fix in): Recovery, results saved
- **Results** (in `private/boot-results-20261104/`, not public):
  - `hw.logicalcpu 24`, `MacPro7,1`, brand string "Intel(R) Core(TM) Ultra 9 285K".
  - diskutil shows only the recovery ramdisk and the stick; no internal disks, as expected behind VMD.
- **VMDProbe v0.0.1:** "scanned; no functions found".
  - VMCAP 0x1, **VMCONFIG 0x2F01: bus-restrict encoding 3**, CFGBAR 256 MB, 32 MSI-X vectors, command 0x6.
  - Linux and FreeBSD both reject encoding 3 ("Unknown Bus Offset Setting (3)"). It's a known open issue on Arrow Lake: kernel bugzilla 221136 (Feb 2026), and a linux-pci thread for an MSI 275HX with 8086:ad0b and the same VMCONFIG low half 0x2F01. That reporter found NVMe on buses 0xE2/0xE3 with "3 → 224" and relative ECAM numbering, so the probe's bus window is probably right.
  - **But macOS relocated the VMD BARs:** CFGBAR went from 0xB8_2000_0000 (firmware/Windows) to **0x8000_0000**, MEMBAR2 from high memory to 0x6500_0000; MEMBAR1 stayed at 0x7200_0000. Whether the new CFGBAR address reaches the VMD is the open question.
  - **VMDProbe v0.0.2** (sha256 fae35fd2…) scans the whole CFGBAR (256 ECAM bus slots, which covers relative and absolute numbering) and counts all-ones, zero and data reads (all-ones means the address isn't decoded). It also records the raw BAR registers. Read-only, on the stick.
- **Dock works without a connection manager:**
  - WD19TB enumerated (Titan Ridge xHCI, AppleUSBXHCITR), with hubs, the "Kick75 IO" keyboard, **"Realtek USB2.0 Audio" as an IOAudio device**, and **dock Ethernet as en1** (AppleUserECM).
  - Both onboard xHCIs (XHCI@14 PCH, TXHC@d TCSS) are on AppleUSBXHCIPCI.
- **Onboard Ethernet:** LucyRTL8125 1.2.4 → `en0`, primary interface, link up at 1 Gbps.
- **Twelfth-attempt config (saves a round trip):**
  - VMDProbe v0.0.2 **plus VMDBridge level 1** (`-vmdbridge`, binary sha256 7a78d810…, unchanged). Level 1 disguises the class as 0x018000, so no storage driver attaches and there is no disk I/O. It refuses cleanly if there is no AHCI function, more than one, an ABI mismatch, or a bad ABAR.
  - Both kexts match 8086:AD0B with score 0 in the default category, so only one would have started. VMDProbe now has its own `IOMatchCategory` (VMDProbe) and co-exists with VMDBridge. Rebuilt and on the stick.
  - Save command now also writes `vmdbridge.txt` (`ioreg -r -c VMDController`). ocvalidate: no issues. Stick config e9e21e93….

## 2026-11-04: Twelfth boot (VMDProbe v0.0.2 + VMDBridge level 1): the VMD window was at the wrong address
- **Results:** Recovery again, 24 CPUs; disks: still only the ramdisk and the stick.
- **VMDProbe v0.0.2:** 8192 reads, **all all-ones**, 0 functions. VMDBridge loaded (`kmutil`) but left no `VMDController` object, so its `start` failed (most likely "no AHCI function", from the same all-ones reads). The failure reason was lost with the freed object.
- **Cause, from the raw BAR registers:** BAR0 = 0x80000004 (64-bit memory BAR), **BAR1 = 0xB8**.
  - IOPCIFamily's `reg` / `assigned-addresses` for the VMD show that it sized the 64-bit CFGBAR (BAR0/1) and MEMBAR2 (BAR4/5) **as 32-bit BARs**, and took BAR1 and BAR5 for I/O BARs it never assigned.
  - It wrote only the low dwords (CFGBAR 0x8000_0000, MEMBAR2 0x6500_0000) and left the firmware's high dword 0xB8.
  - So the VMD decoded **0xB8_8000_0000** while macOS mapped and reserved 0x8000_0000, where nothing answers. Every probe read went to empty address space. MEMBAR1 (32-bit, 0x7200_0000) is consistent, so the AHCI ABAR inside it should be intact.
- **Fixes, for the thirteenth boot:**
  - **VMDBridge v0.1.1** (sha256 0988ca59…) clears the BAR1/BAR5 high dwords before anything else, so the hardware decodes exactly where IOPCIFamily allocated and reserved. It does this only if the BAR is 64-bit typed, IOPCIFamily's range is below 4 GB and equal to the low dword. Memory decode is off during the write. The register values before and after, and any failure reason, are now written onto the VMD's own registry node (`VMDBridgeStatus`), so they survive a failed start.
  - **VMDProbe v0.0.3** (sha256 fdcb0568…, still read-only) maps the address the hardware actually decodes, `(BAR1 << 32) | BAR0`, and records it next to IOPCIFamily's address (`VMDCfgBarHWPhys` / `VMDCfgBarOSPhys`).
  - **One-word save:** the stick now holds a `go` script (`tools/go` in the repo). In Recovery Terminal: `sh /Volumes/OPENCORE/go`. It also saves the VMD node with both drivers, `dmesg` and the kernel log lines that mention VMD.

## 2026-11-04: Thirteenth boot (VMDProbe v0.0.3 + VMDBridge v0.1.1): first look behind the VMD
- **VMDProbe v0.0.3 mapped the address the hardware decodes (0xB8_8000_0000) and found the VMD domain.** Relative bus numbering with VMCONFIG encoding 3 → bus 224 is confirmed:

  | ECAM slot (bus) | Device | What |
  |---|---|---|
  | 0 (224) | 00:01.0 8086:7ECC, class 060400, secondary bus 226 | root port |
  | 1 (225) | 17.0 8086:7F62, class 010601 | **SATA AHCI (Samsung 860 EVO / HDD)**, ABAR (BAR5) 0x7280_0000, inside MEMBAR1; BAR2–4 legacy I/O, unassigned |
  | 2 (226) | 00.0 1C5C:1069 (SK hynix), class 010802 | the Windows NVMe, BAR0 0x7200_0000 |

  Slots 128 and 226 return aliases of slots 1 and 2. The AHCI function has capabilities MSI (0x80), PM (0x70) and SATA (0xA8).
- **VMDBridge v0.1.1** loaded after the probe and cleared both high dwords (`VMDBAR1Before/VMDBAR5Before` = 0xB8, after = 0). It then refused with "no AHCI function on the VMD root bus". It only searched slot 0, and the AHCI function is on slot 1 (bus 225). It is not behind a bridge there, it is just placed on the next bus.
- No internal disks yet (expected: nothing was exposed).
- **VMDBridge v0.1.2** (sha256 ac80ac4c…), for the fourteenth boot:
  - searches ECAM slots 0–31 for the AHCI function and remembers its real slot. IOPCIFamily sees it on the one virtual bus at 17.0, still disguised (level 1), and the NVMe and root port stay hidden.
  - hides the AHCI's unassigned legacy I/O BARs and ROM (read 0, writes ignored), so IOPCIFamily only places the ABAR.
  - checks that the cleared address (0x8000_0000) actually answers. If not, it writes the firmware high dwords back and uses the firmware address, which the probe proved works. Which path it took is recorded as `VMDBridgeCfgPath`, and level 2 refuses the fallback path.
  - records on the VMD node: functions seen, the AHCI slot/device, the ABAR and the final status.
- `log show` doesn't work in Recovery (no logd), so `vmdlog.txt` is empty; `dmesg.txt` holds the kernel log.
- **Shutdown fix (boot 13):** after Apple menu → Shut Down the PC appears to have powered off instead of restarting (user report, not certain). SSDT-FIXSHUTDOWN looks like it works; check once more.

## 2026-11-04: Fourteenth boot prepared as the first install attempt (VMDBridge 0.2.0, level 2)
- The test boot (level 1, class disguised) is skipped at the user's request. The next boot goes straight to level 2 and the macOS install on the Samsung SSD. Guide: `INSTALL.md`.
- **VMDBridge 0.2.0** (sha256 f2afc5e2…):
  - the interrupt path now addresses the AHCI function on its real ECAM slot (`childBase` used slot 0);
  - review finding 6: one child's handler never runs on two CPUs, and an edge that arrives while it runs is re-run;
  - review finding 7: the child is published under the lock;
  - review finding 8: the Extended Message Data Enable bit is cleared.
  - Findings 1–3 (pending replay, quiescing disable/unregister, MSI re-programmed after config restore) were already in the code. Finding 5 is handled by v0.1.2 hiding BAR0–4 and the ROM.
- **Stick config:**
  - `boot-args` `-v keepsyms=1 debug=0x100 -lilubetaall vmdbridge=2` (VMDProbe stays loaded but inert);
  - **ScanPolicy 0x370002**: SATA, SAS, SCSI, ATAPI and USB devices; NVMe and generic PCI excluded, so the Windows NVMe is never offered;
  - **ShowPicker on, Timeout 10**, so the installer's restarts are visible and boot the default entry by themselves.
  - ocvalidate: no issues.
- **Checks before the erase:**
  - Windows' WinRE is on the NVMe (ReAgent.xml offset ≈1880.8 GB, the 2 TB disk). The Samsung's Recovery/EFI partitions are leftovers.
  - Every folder still on D: was compared again with its copy on E: (file count and bytes identical).
- **Restart fix staged:** under macOS, Restart powered the PC off. The FADT reset register is Dell's SMI port (I/O 0xB2, value 0x73). An OpenCore ACPI patch on FACP (macOS only) points it at the PCH Reset Control register (I/O 0xCF9, value 0x06, hard reset). The firmware's boot default is Windows, so INSTALL.md now says to use F12 → USB at every installer restart.
- **Boot 14 (attempt):** with USB first in the BIOS boot order, the OpenCore picker listed **Windows**, defaulted to it and got stuck. The OC log on the stick was empty (power cut before it was flushed). The Windows entry most likely came from the old Windows boot files on the not-yet-erased Samsung, which the SATA-enabled ScanPolicy now reaches. **Fix:** two configs on the stick. Recovery mode (default: USB only, no picker) boots straight to Recovery. `sh /Volumes/OPENCORE/installmode`, run after erasing the Samsung, switches to install mode (SATA too, picker 10 s). `build-efi.sh` now writes both, plus the scripts.

## 2026-11-04: Fourteenth boot (VMDBridge 0.2.0, level 2): **macOS read both SATA disks behind VMD**
- `VMDBridgeStatus` "bridge up (level 2)", config path "IOPCIFamily range (high dword cleared)": the cleared CFGBAR address decodes. AHCI at slot 1, device 0x17; 3 functions seen.
- **AppleAHCI attached to the exposed 8086:7F62**, with ports PRT4 and PRT7 holding disks:
  - **WDC WD60EFZX (disk1)**, 6001175126016 bytes: GPT with one basic data partition.
  - **Samsung SSD 860 EVO 500GB (disk2)**, 500107862016 bytes: GPT with Recovery, EFI, MSR, basic data, Recovery.
  - 11 reads per disk (the partition tables), 0 errors, 0 retries, 0 writes, total read time 13.6 ms and 1.7 ms. **VMD interrupts and DMA work.**
- **But DiskArbitration never published them.** `diskutil list` and Disk Utility showed only the ramdisk and the stick (disk0, disk3), and no further reads were issued, so DA never probed them. Cause not yet known; the IOMedia objects are registered, matched and not busy. AppleAHCIPort reports "Physical Interconnect Location" External.

## 2026-11-04: VMD switched off; install config without VMD drivers
- **The user switched SATA/NVMe Operation to AHCI/NVMe in the BIOS**, using the Safe Mode route: `bcdedit /set "{current}" safeboot minimal`, the BIOS change, then `bcdedit /deletevalue "{current}" safeboot`. In PowerShell `{current}` must be quoted.
- **Windows boots normally:** "Standard SATA AHCI Controller" 8086:7F62 and "Standard NVM Express Controller" for the SK hynix.
- **Config:** VMDProbe and VMDBridge are disabled and `vmdbridge`/`-vmdprobe` are gone from boot-args; recovery and install modes are otherwise unchanged (ocvalidate clean). The VMD work stays in the repo for anyone who has to keep VMD on.

## 2026-11-05: Install stage 1 done; stage 2 needs the stick in macOS mode
- **What happened overnight:** the Samsung now holds an APFS container (464 GB, partition 4). The old Recovery/EFI/MSR partitions 1–3 were kept, because only the data partition was erased.
  - `installmode` was never run, so the stick stayed in recovery mode (USB only), and the installer's restart into "macOS Installer" returned to Recovery.
  - Recovery then went to sleep with the display off and could not wake (the firmware has S0ix only).
- **New stick:** a Lexar 64 GB (FAT32, MBR, labelled OPENCORE) with the same EFI, kexts and recovery image (verified with `diff -r`/`cmp`). Its factory DataShield files are left in place. Its `config.plist` is **macOS mode**:
  - ScanPolicy 0x370303: FS lock with APFS+HFS only; SATA, SAS, SCSI, ATAPI and USB; no NVMe. The Samsung's leftover Windows ESP can no longer create a Windows entry.
  - HideAuxiliary on; picker 5 s.
- The old stick keeps recovery mode as a fallback. `tools/go` now also turns sleep off (`pmset -a sleep 0 standby 0 hibernatemode 0 powernap 0 disksleep 0`) and saves `sw_vers`, `pmset` and a system_profiler summary.
- **Picker stuck (2026-11-05):** the macOS-mode picker on the Lexar showed, ignored the keyboard and never timed out. Its log was empty again (power cut before the flush). Now **ShowPicker false** (OpenCore boots the default entry, the macOS Installer, with no input) and **KeySupport false** (use the firmware's keyboard input instead of OpenCore's own driver) in both modes.

## 2026-11-05: **macOS Tahoe installed on the Samsung and booting from it**
- The Lexar in macOS mode (no picker) booted the macOS Installer stage and then the installed system, with no input needed.
- The user reports: **keyboard, sound (speakers), and internet work**.
- **The UI is very slow**, as expected: no GPU acceleration (the plain firmware framebuffer, drawn by the CPU) until the RTX 5090 driver is installed.
- The setup assistant hung for about 15 minutes on the "Automatic Updates" screen after choosing "Download only"; the user restarted. Still to do: finish setup, then run `go` (results and sleep off).

## 2026-11-05: RTX 5090 driver staged (NullMoth 2.7.1, not yet installed)
- `nullmoth-nvidia-2.7.1.tar.gz` (sha256 0b28f420…): **Ed25519 signature verified** with the release key, GitHub digest and SHA256SUMS match, and all 77 inner files OK. `install.sh` equals repo 6b4752c. The 2.6.6→2.7.1 diff adds NMXAudio (macOS 26, HDA present) and a chmod. 2B85 is on its SUPPORTED_IDS list.
- AMFIPass 1.4.1 from OCLP payloads (commit 76516394), zip sha256 07b26614…961e, the same pin as NullMoth's 1401.
- The 5090 BARs in macOS sit above 4 GB (BAR1 0xA0_1000_0000, 64 MB small BAR; the driver resizes it itself).
- `config-nvidia.plist` comes from macOS mode via `tools/make-postinstall-config.py`, whose IONDRVSupport block now uses Strategy **Exclude** as the driver README asks. ocvalidate is clean.
- On the Lexar: `nvidia/` (tarball + sig), `EFI/OC/Kexts/AMFIPass.kext`, `config-nvidia.plist`, `config-basic.plist`, and the scripts `gpu`/`gpuoff`. `gpu` switches the config only after `install.sh` succeeds. Guide: `GPU.md`.
- The old 4 GB stick still has to be turned into the basic-display fallback (it was not plugged in).

## 2026-11-05: Boot broke after the macOS-side iGPU experiments; Lexar put back on the driver config
- A Claude session running inside macOS added things to the Lexar: `hidpi`, `tune`, `IGPUProbe.kext` (`-igpuprobe`), Bluetooth (IntelBTPatcher 2.5.0 built from master, IntelBluetoothFirmware, BlueToolFixup) and finally `IGPUScanout.kext` with `-igpuscanout`. After that macOS no longer booted.
- All five OpenCore logs since then reach `EXITBS:START`, so OpenCore is fine and the stall is in the kernel or later.
- **Restored** `config.plist` = the session's `config-nvidia.plist`: NullMoth driver plus `nvfbhidpi=1`; no iGPU kexts, no Bluetooth, ocvalidate clean. The broken one is kept as `config-broken-igpuscanout.plist`. Nothing on the Samsung was touched.
- **Bluetooth added back at the user's request:** `config.plist` = `config-nvidia.plist` plus BlueToolFixup 2.7.2, IntelBluetoothFirmware 2.4.0 and IntelBTPatcher 2.5.3 (built from master by the macOS-side session; not pinned by this project). No iGPU kexts. ocvalidate clean. Also saved as `config-nvidia-bluetooth.plist`; the fallback without Bluetooth is `config-nvidia.plist`.

## 2026-11-05: Real cause of the boot failure: IntelBTPatcher 2.5.3, not the iGPU kext
- Read the Mac data volume from Windows, read-only: a throwaway WSL distro `apfsreader` (Ubuntu 24.04 base image), apfs-fuse @66b86bd5, `wsl --mount --bare` on the Samsung. No FileVault.
- Copied to `private`/`from-mac`: the macOS session's `igpu-driver/` (IGPUProbe, IGPUScanout, lgmirror, lgmst, igpuctl, README), `~/bin`, its EFI backup and DiagnosticReports.
- **Timeline** (Mac time PST; OpenCore log names use the RTC, which is PST+8):
  - The boot at 12:32 (BT + probe) ran for 4.6 h. Then at 17:35 a **panic**: WindowServer watchdog, `WindowServer failed to spawn with error 5`, with root disk errors **"Could not recover SATA HDD after 5 attempts"**. The Samsung dropped off AppleAHCIPort. Windows reports it healthy; cause still open.
  - Boots at 17:35, 17:46 and 17:58 **with IGPUScanout 0.0.3 and `-igpuscanout`** reached the desktop (diag reports written, then shutdown stalls).
  - At 18:50 the session replaced IntelBTPatcher 2.5.0 with **2.5.3**, keeping a full EFI backup first. The 19:00 boot failed.
  - The backup EFI and the failing stick differ **only** in `IntelBTPatcher.kext`.
- **Fix:** the stick now equals that backup (IntelBTPatcher 2.5.0, IGPUScanout, IGPUProbe, Bluetooth, NVIDIA). 2.5.3 is kept in `Kexts-backup/IntelBTPatcher-2.5.3.kext`; the config is also saved as `config-working-igpu-bt.plist`.
- **Open:** the 17:35 SATA root-disk drop (AppleAHCIPort, Samsung 860 EVO on the Arrow Lake PCH 7F62).

## 2026-11-05: Why macOS stopped booting again (read from the Mac's own unified log)
- Tools: Mandiant unifiedlog_iterator 0.7.0 (release sha256 7192a187…, user-approved) in the throwaway `apfsreader` WSL distro, on the read-only apfs-fuse mount of the Samsung data volume.
- **Failed boot (log 09C29228, about 08:41Z):** the disk is fine (AHCI ports 4/7 up, APFS mounted). NVRM brought the 5090 up (PASS 2, GSP-RM running, 32 GB VRAM, BAR1 resized to 8 GB at 0x3FC00000000) and published 4 NVRMDisplay nubs at 47.0 s. Then **no "boot guard cleared" and no "boot hold RELEASED"**, and the log stops at 47.8 s: the machine froze while NVRMFB took over head 0. The boot guard logged "start 2 without a display so far", so the next start would turn the driver off.
- **Good boots (E05317CF, F2C999EE, F8C6361D):** the same sequence, plus "boot guard cleared: the framebuffer has its scanout memory and a display mode" and "boot hold RELEASED". The one difference seen: `headwatch: head 1 RUNNING` is followed by `head 1 STOPPED` within 25 ms in good boots, but not in the failed one.
- The on-screen symptom (repeated `AppleKeyStoreUserClient failed MACF`) matches the NullMoth TROUBLESHOOTING entry "Loops at AppleKeyStore after installing the driver… card firmware needed more time before the driver turned the display on", whose fix is `nvrmsettle=10000` (the default settle is 0.5 s for non-Turing cards).
- **Staged fix** (the Lexar was unplugged; staged in `private/lexar-staging`): `nvrmsettle=10000` plus `nullmoth-nv-starts` in NVRAM → Delete (7C436110), so the guard's count of 2 does not switch the driver off on the test boot. ocvalidate clean.
- **Bluetooth works** with IntelBTPatcher 2.5.0: 8087:0036 on HS14, BlSt PoweredOn, firmware v17222, LMP 13 (5.4), GATT connections. In one session it went PoweredOff after 3 minutes.
- **USB:** macOS uses the first 15 xHCI ports (HS01–HS14 + SS01). `kexts/USBMapAAT2250.kext` (codeless, from the Dell `_UPC`/`_PLD`) keeps all HS ports except HS13 and adds SS09/SS10. Staged as a separate step (`usbmap`).
- **Wi-Fi BE200:** only AirportItlwm-Tahoe 1.2.0 BE200 Alpha 2 (GPL-2.0), which needs OCLP-CustoMac root patching of the sealed system volume. Not done without the user.
- **Login screen in a quarter of the panel:** NVRMFB keeps the firmware's 2560x1440 boot raster until WindowServer applies the user's 1920x1080@2x mode at login. No driver switch exists; firmware 4K output is a later experiment.
- **Icons:** NullMoth issue #89 (Icon Composer icons on macOS 26). **Minecraft:** OpenGL is translated (issues #261/#306).
- **Clock:** in-boot timestamps jump by 8 h, so the RTC holds local time (Windows default) while macOS reads it as UTC.

## 2026-11-05 — boot fix copied to the Lexar

- Backed up the stick's config.plist (= config-working-igpu-bt) as `EFI/OC/config-before-nvrmsettle.plist`.
- New `EFI/OC/config.plist` = config-nvrmsettle (working iGPU/BT config + `nvrmsettle=10000`, boot-guard NVRAM key cleared each boot).
- Also copied, not active yet: `config-usbmap.plist`, `Kexts/USBMapAAT2250.kext`, `/usbmap` script (`sh /Volumes/OPENCORE/usbmap`, `... usbmap off` to undo).
- All files verified with cmp/diff against staging.
- Rollback: on Windows copy `config-before-nvrmsettle.plist` over `config.plist`.
