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

**Non-destructive copy:** started copying D:\{backup files, MSFS 2024 add_ons, Virtual Machines, BeamNG Drive} → `E:\D-migration\` with robocopy (no deletes). Logs are in `evidence/d-to-e-copy/`.
- **D:→E: copy verified complete** (robocopy exit 1 = "files copied"). Results:

  | Folder | Files | Size |
  |---|---|---|
  | MSFS 2024 add_ons | 1148/1148 | 0.99 GB |
  | Virtual Machines | 63/63 | 51.90 GB |
  | BeamNG Drive | 13982/13982 | 48.22 GB |
  | backup files | 54 dirs | 0 files |

  - 0 failures in every log. D: is unchanged.
  - Still to move with their own tools (user or admin): XboxGames/WindowsApps (Xbox app), SteamLibrary (Steam), WSL NVIDIA-Workbench (`wsl --manage --move`), pagefile (System Properties).
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
