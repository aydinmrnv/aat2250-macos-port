# First test boot: Tahoe recovery USB with VMDProbe

> **Status 2026-11-02: the USB stick is READY.**
> - **Stick:** 3.8 GB, F:, FAT32, label OPENCORE.
> - **Contents:** OpenCore EFI + `com.apple.recovery.boot` (Tahoe recovery BaseSystem.dmg, 916 MB, Apple chunklist verified). All 39 files are hash-verified against their sources.
> - **Why recovery, not the full installer:** the stick is too small for Apple's full installer (needs 16 GB+). The recovery image gives the same Terminal for this test, and it can later install macOS over Ethernet.
> - **Skip Step 1 below.** Go straight to Step 2.

**Goal of this boot:** no install and nothing written to any internal disk. Boot the Tahoe installer from USB on the Alienware and read what the VMD driver's phase 0 sees. That one result decides whether the internal-D: plan works.

## What exists now (2026-11-02)
- **Installer:** Apple Tahoe 26.7.2 (25G332) full installer is being fetched to `/Applications/Install macOS Tahoe.app` on the Mac Studio with `softwareupdate`.
- **Installer-test EFI:** at `~/macos-port-build/efi-stage/EFI-installer-test/EFI` on the Mac Studio.
  - OpenCore 1.0.8 and the pinned kexts (each zip SHA-256-verified).
  - The three ACPI tables, plus VMDProbe and VMDBridge.
  - `ocvalidate` 1.0.8: **no issues**.
  - boot-args: `-v keepsyms=1 debug=0x100 -vmdprobe`. VMDBridge stays inert because there is no `vmdbridge` boot-arg.
  - NVIDIA, AMFI and Bluetooth are off.
  - The config holds generated SMBIOS values for this machine, kept in `private\` (not shared, not in deliverables).
- **Not boot-tested:** none of this has been booted yet.

## Step 1: make the USB (Mac Studio, you run the admin steps)
1. Plug the blank USB stick into the Mac Studio. Claude will list it with `diskutil list external physical` (read-only) and show you its identifier, size and model.
2. **Check that identifier yourself.** The next command erases it.
3. Run in Terminal on the Mac, replacing `/Volumes/USB` with the stick's volume name:

   ```bash
   sudo "/Applications/Install macOS Tahoe.app/Contents/Resources/createinstallmedia" --volume /Volumes/USB
   ```
4. Put the EFI on the stick's EFI partition, replacing `diskN` with the stick's identifier:

   ```bash
   sudo diskutil mount diskNs1
   ```

   ```bash
   cp -R ~/macos-port-build/efi-stage/EFI-installer-test/EFI /Volumes/EFI/
   ```

   ```bash
   diskutil unmount /Volumes/EFI
   ```

## Step 2: before plugging it into the Alienware (your decisions; nothing here is done by Claude)
- **BitLocker:** run `manage-bde -status` in an admin terminal. If any volume is protected, make sure you can get to the recovery key, and suspend BitLocker before changing any firmware setting.
- **Secure Boot:** OpenCore with this config will not start while firmware Secure Boot is on (see `a2-platform/platform-analysis.md` §4 for the options and their BitLocker risk). Turning it off is a firmware change that only you make, and it can be turned back on afterwards.
- **Backups:** the VMD driver only reads in this test, but have backups of C: and of D:. D:'s plain folders are already copied to `E:\D-migration`.
- **Do not change** VMD/RAID/AHCI, VT-d or anything else.

## Step 3: the test boot
1. Plug the USB stick into a rear motherboard USB port, not the dock.
2. Press F12 at power-on and choose the USB stick. In the OpenCore picker, choose the **macOS Base System / recovery (dmg)** entry.
3. Watch the verbose text. If it stops, photograph the last screen. That photo is the test result.
4. If recovery reaches its menu, open Utilities → Terminal and run the following. The stick is mounted at `/Volumes/OPENCORE`:

   ```bash
   ioreg -l -w0 -r -c VMDProbe > /Volumes/OPENCORE/vmdprobe.txt
   ```

   ```bash
   sysctl hw.logicalcpu hw.model >> /Volumes/OPENCORE/vmdprobe.txt
   ```
   Then run the USB4Probe readout (phase D1, read-only; boot-arg `-usb4probe` is already in the config):

   ```bash
   ioreg -l -w0 -r -c USB4Probe > /Volumes/OPENCORE/usb4probe.txt
   ```

   Also run this one. It captures the dock and iGPU data for `DOCK-AND-IGPU-PLAN.md` phase D0. The file can be several MB and stays on the stick.

   ```bash
   ioreg -l -w0 > /Volumes/OPENCORE/ioreg-full.txt
   ```
   Leave the WD19TB dock connected as it normally is, but boot from the rear port, not the dock. Note which monitors show anything at all during boot: the S3225QS, the LG on the dock, the LG on the direct cable.
5. Shut down. **Do not run the installer.** Boot Windows normally to confirm it is untouched, and bring the USB back so Claude can read `vmdprobe.txt`.

## Pass/fail for this boot
| Check | Pass |
|---|---|
| OpenCore picker appears | Yes |
| Recovery reaches its menu | Yes. Otherwise the verbose-screen photo shows where it stops (CPU spoof, ACPI, MADT x2APIC, display) |
| `hw.logicalcpu` | 24 |
| `VMDStatus` | `scanned` |
| `VMDTopology` | One function of class `0x010601` (SATA AHCI) on bus `VMDBusStart`; the NVMe behind a type-1 bridge |
| `USB4ProbeStatus` | `done`. `NHIStatus` = `read`. `NHIFirmwareCM` is recorded either way, since both answers are results. |
| Windows afterwards | Boots normally, unchanged |

## Rollback
Remove the USB stick. Nothing on the internal disks changed. If you turned Secure Boot off, you can turn it back on. Windows Boot Manager stays the firmware default, because the config never writes boot entries (`RequestBootVarRouting` on, `LauncherOption` disabled).
