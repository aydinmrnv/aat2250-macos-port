# Decisions log

| Date | Decision | Made by | Rationale | Reversal |
|---|---|---|---|---|
| 2026-11-02 | Target **macOS Tahoe 26.7.2 (25G332)** | User | User preference. The BE200 Wi-Fi alpha was tested on 26.7.2. | Change `#TARGET`, then re-check audio and Bluetooth |
| 2026-11-02 | **Audio phase 1: USB audio**, AppleALC disabled | Claude (user delegated) | Tahoe has no AppleHDA, and restoring it adds more SIP reduction plus a redo after every update. USB class-compliant audio needs no patching. | Phase 2: restore AppleHDA + enable AppleALC + find layout-id |
| 2026-11-02 | **SSD D (Samsung 860 EVO) via a USB 3 SATA enclosure** (recommended route) | Claude (user delegated) | All internal disks sit behind VMD. A macOS VMD driver would also expose the Windows SED NVMe on the same VMD domain to an experimental kernel driver. An enclosure needs no new driver, and moving the disk back is just reinstalling it internally. | A VMD driver project, only after macOS boots from USB and is used as the dev host |
| 2026-11-02 | **SSD D stays internal, macOS installed on it**; enclosure route rejected; D: data to move to E: | User | User wants macOS on internal D. Still blocked by VMD: macOS cannot see any internal disk while VMD is on | — |
| 2026-11-02 | Build untrusted NVIDIA build scripts only in a VM on the Mac Studio | Claude | Scripts assume private paths and ssh to the maintainer's build host | — |

## Actions only the user can take for these decisions
- **SSD D:**
  - Back up everything on D: (the 463 GB NTFS partition plus its two recovery partitions and its EFI partition) to a location you trust. Then check the backup.
  - Buy a USB 3.x 2.5" SATA enclosure (UASP preferred).
  - Erasing the disk happens only after you confirm the backup, and it needs your explicit go-ahead at that time.
  - Note that D: holds an EFI and recovery partitions. Check whether anything boots from them before removing the disk (Windows boots from Disk 0's EFI).
- **USB audio:**
  - The Realtek USB audio is inside the WD19TB Thunderbolt dock, whose macOS behaviour is unknown.
  - A small class-compliant USB DAC/headset on a motherboard USB port is the low-risk fallback.

## Release pins (OpenCore Kernel→Add), verified 2026-11-02
| Package | Tag | SHA-256 of release zip | Source of hash |
|---|---|---|---|
| OpenCore-1.0.8-RELEASE.zip | 1.0.8 | 2011e8b7216ecb2645d97ea710df965947edd406846e92050b9bc4190be6d27b | GitHub digest = A2 local |
| Lilu-1.7.2-RELEASE.zip | 1.7.2 | 53967d7dcfaab01023a33df2e969a89522f13d6654a6a56ac4711b62dabf3ab8 | GitHub digest |
| VirtualSMC-1.3.8-RELEASE.zip | 1.3.8 | 2e29a8aaf91b4eb0bcdf5913ed17e70f5cf76531122acd4132eff7813a54de8b | GitHub digest |
| RestrictEvents-1.1.6-RELEASE.zip | 1.1.6 | 98170dfae195ddd28b5d95e3f040125a13ca783bcb9bd1e5b8c588e217b14ee6 | GitHub digest |
| CpuTopologyRebuild-2.0.2-RELEASE.zip | 2.0.2 | c25d07464d2c9adf120dbf13a50321ff1bdb273290a5e82235eef4740db7b911 | hashed locally (no digest) |
| NVMeFix-1.1.3-RELEASE.zip | 1.1.3 | e1d5657ab7ac31f69771708f7b80bf218ab9aa0b8e4c4fe6ff943983037e3dfb | GitHub digest |
| AppleALC-1.9.8-RELEASE.zip (phase 2) | 1.9.8 | d4d36cd2da7863cf7cbfcb0892d45c08457f4f4ddd065a073dd010e46bd63bcb | GitHub digest |
| LucyRTL8125Ethernet-V1.2.4.zip | v1.2.4 | a4e24a0246fff0bc059be4b832b7e4c800261364868632df2f38a2bf998e3956 | GitHub digest |
| BrcmPatchRAM-2.7.2-RELEASE.zip (BlueToolFixup) | 2.7.2 | e1c1c55347526d031a8ae2fdd1f52efa3019161e497fb38e1cfa809752f8af21 | GitHub digest |
| IntelBluetooth-v2.4.0.zip | v2.4.0 | 78418daa11f620012fc53ba68cbd643c2642a45f2f96c594fd01aaf2bc68fe2b | hashed locally; release plist has `0036` personality |
| USBToolBox-1.2.0-RELEASE.zip | 1.2.0 | c315a3a5acfd496dd97d0d19b4fbd1d487103d2fd541c5651583d4c9cebcfe07 | GitHub digest |

**Still open:**
- **IntelBluetoothFirmware on Tahoe:** v2.4.0 (2024-02) predates its Tahoe commit (A3), so a Tahoe build may be needed. It could be built from source on the Mac Studio.
- **AMFIPass:** source, license and hash are unreviewed.
- **HfsPlus.efi:** pin OcBinaryData commit `640e7e92…` (file last changed in `c2a98980`).
- **USB map:** still needs physical port testing.

