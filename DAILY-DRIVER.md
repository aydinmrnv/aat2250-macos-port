# Daily-driver roadmap (AAT2250 on macOS Tahoe)

Where the port stands, and what each remaining piece is, in the order it gets done. Updated 2026-11-04.

## Working
- **Boot:** OpenCore → Tahoe Recovery, with the keyboard and mouse working (ten test boots; see `deliverables/PROGRESS-LOG.md`).
- **CPU:** all 24 cores registered after the MADT rewrite (`deliverables/acpi/x2apic_to_lapic.py`).
- **ACPI:** Dell's Arrow Lake reference code fixed for macOS's ACPICA 20160930 (`deliverables/acpi/dell_dsdt_patches.py`, 9 patches).
  - The patched DSDT plus all SSDTs macOS sees load cleanly in `acpiexec`: 28 tables, 0 load errors.
- **EC:** the fake `EC` plus USBX now load, and Dell's real EC is hidden from macOS.
- **Ethernet:** onboard Killer E3100G = `en0` (LucyRTL8125), link up at 1 Gbps; the dock's Ethernet = `en1` (Apple's own driver).
- **Dock (WD19TB) at boot:** USB, the dock's USB audio (shown as an audio device) and Ethernet all work with Apple's drivers, because the firmware sets up the Thunderbolt tunnel. Hot-plug still needs a connection manager.

## Staged (in `efi/config.plist`, not yet boot-tested)
- **Shut Down powers off instead of restarting:** `SSDT-FIXSHUTDOWN-AAT2250` plus a `_PTS → ZPTS` rename. It clears xHCI `PME_En` at S5 under macOS, the same fix as on the Arrow Lake Legion 5.
- **iCloud / iMessage readiness:** `ROM` = the onboard Ethernet MAC. This is set in the private config only; `build-efi.sh` still uses a random ROM.

## Next, in order
| # | Piece | State | Needs |
|---|---|---|---|
| 1 | **Internal disk (D:, Samsung 860 EVO)** | D: emptied (data on E:). Boot 12: macOS left the VMD's 64-bit BARs half-programmed, so every probe read hit empty space. Boot 13: SATA AHCI found behind VMD (bus 225, 17.0, ABAR 0x7280_0000). VMDBridge v0.1.2 exposes it (level 1, disguised) | Probe result → VMDBridge level 1/2 tests → install macOS |
| 2 | **Install macOS** on D: | — | 1, plus the user's explicit go-ahead to erase D: |
| 3 | **RTX 5090 acceleration** (native 4K@120, Metal) | `tools/make-postinstall-config.py` derives the config (AMFI, IONDRV block, IOMMU quirk) | 2; review and pin AMFIPass; install the nullmoth driver package; test BAR1 32 GB first |
| 4 | **USB port map** (the PCH xHCI has 26 ports, macOS allows 15) | waiting for `ioreg-full.txt` | a codeless map kext (no USBToolBox: it stalled the boot) |
| 5 | **Shut Down / restart** | staged (above) | a boot to confirm |
| 6 | **Audio** | the dock's USB audio works in Recovery | onboard analog needs AppleHDA, which Tahoe dropped; use the dock or a USB DAC |
| 7 | **Bluetooth** (Intel 8087:0036) | IntelBluetoothFirmware v2.4.0 predates Tahoe | a Tahoe build from source (needs the user's OK) |
| 8 | **Wi-Fi** (BE200) | only an alpha driver exists | Ethernet is the daily path |
| 9 | **Dock** (WD19TB) | its xHCI 15F0 was visible at boot (firmware sets up the tunnel) | USB and Ethernet to be checked; hot-plug needs the USB4 connection manager (`deliverables/DOCK-AND-IGPU-PLAN.md`) |
| 10 | **iGPU monitors (LG ×2)** | no driver for external displays on 7D67 | cable them to the 5090 meanwhile |
| 11 | **Sleep** | the firmware has S0ix only | turn sleep off after install (`sudo pmset -a sleep 0 hibernatemode 0 standby 0`) |
| 12 | **CUDA** | the loader works without a GPU (`cuda-shim/`) | 3, then the device shim and a UVM kext |
