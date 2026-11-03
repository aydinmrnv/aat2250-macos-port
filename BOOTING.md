# Booting the test USB on the Alienware Area-51 AAT2250

**What this boot is:** a **diagnostic boot**, not an install.
- It starts the macOS Tahoe recovery environment from a USB stick.
- It runs three read-only probe drivers (VMDProbe, USB4Probe, plus the inert VMDBridge) and saves their results to the stick.
- **Nothing is written to any internal disk.** Windows stays the default boot.

Nothing in this repo has been boot-tested yet. The boot may stop partway, and that is still a useful result (see step 5).

You need:
- a Mac (any recent macOS, Intel or Apple Silicon) to build the stick;
- a USB stick of **2 GB or more** that you are happy to erase;
- the Alienware.

---

## Step 0: decisions and checks on the Alienware (Windows). Do these yourself.
1. **Back up** anything you care about. The test only reads, but backups come first.
2. **BitLocker:** open **Terminal (Admin)** and run:

   ```powershell
   manage-bde -status
   ```
   If any volume shows *Protection On*:
   - make sure you have that volume's recovery key saved (account.microsoft.com → Devices → BitLocker keys);
   - then suspend protection for one reboot before changing firmware settings:

   ```powershell
   manage-bde -protectors -disable C: -RebootCount 2
   ```
3. **Secure Boot must be off** for this OpenCore config.
   - Reboot, press **F2** for BIOS setup, and find **Secure Boot** (usually under *Boot Configuration* or *Security*). Set it to **Off**, then save and exit.
   - It can be switched back on afterwards. Changing it can trigger BitLocker recovery, which is why step 2 comes first.
4. **Change nothing else in BIOS.** Leave VMD/RAID, VT-d and the storage mode exactly as they are. Windows depends on them.

## Step 1: build the EFI folder (on the Mac)

```bash
git clone https://github.com/aydinmrnv/aat2250-macos-port.git
```

```bash
cd aat2250-macos-port && chmod +x tools/*.sh
```

```bash
tools/build-efi.sh ~/aat2250-efi
```

The script:
- downloads OpenCore 1.0.8 and the pinned kexts, and stops if any SHA-256 differs;
- generates your own SMBIOS serials into `~/aat2250-efi`;
- ends with `ocvalidate`, which must print **No issues found**.

**Do not share or commit `~/aat2250-efi`, because it contains your generated serials.**

## Step 2: download the Tahoe recovery image (on the Mac, about 1 GB)

```bash
tools/get-recovery.sh ~/aat2250-recovery
```

## Step 3: prepare the USB stick (on the Mac). This ERASES the stick.
1. Plug in the stick, then list external disks:

   ```bash
   diskutil list external physical
   ```
2. Find your stick by its **size and name**, for example `/dev/disk4`. **Double-check it.** The next command wipes whatever disk you name.
3. Erase it as FAT32, replacing `disk4` with your stick:

   ```bash
   diskutil eraseDisk FAT32 OPENCORE MBRFormat /dev/disk4
   ```
4. Copy the files:

   ```bash
   cp -R ~/aat2250-efi/EFI /Volumes/OPENCORE/
   ```

   ```bash
   cp -R ~/aat2250-recovery/com.apple.recovery.boot /Volumes/OPENCORE/
   ```
5. Eject:

   ```bash
   diskutil eject OPENCORE
   ```

The stick should now hold `EFI/` and `com.apple.recovery.boot/` at its top level.

## Step 4: boot it (on the Alienware)
1. Shut down. Leave the **WD19TB dock connected** the way it normally is: its state is part of the test.
2. Plug the stick into a **rear motherboard USB-A port**, not the dock and not a front port.
3. Power on and tap **F12**. Choose the USB stick, listed as **UEFI: <stick name>**.
4. The OpenCore picker appears. Choose **macOS Base System** (the recovery `.dmg` entry). Do not pick Windows here.
5. Verbose text scrolls. This takes a few minutes.
   - **If it stops or reboots,** photograph the last screen. That photo is the test result. Go to step 6.
6. While it boots, note **which monitors show anything at all**: the S3225QS on the 5090, and the two LGs on the iGPU.

## Step 5: collect results (in macOS Recovery)
1. If a language screen appears, pick one. Then choose **Utilities → Terminal** from the menu bar.
2. Run these four commands. The stick is mounted at `/Volumes/OPENCORE`:

   ```bash
   ioreg -l -w0 -r -c VMDProbe > /Volumes/OPENCORE/vmdprobe.txt
   ```

   ```bash
   ioreg -l -w0 -r -c USB4Probe > /Volumes/OPENCORE/usb4probe.txt
   ```

   ```bash
   sysctl hw.logicalcpu hw.model >> /Volumes/OPENCORE/vmdprobe.txt
   ```

   ```bash
   ioreg -l -w0 > /Volumes/OPENCORE/ioreg-full.txt
   ```
3. Shut down from the Apple menu. **Do not open "Install macOS" or Disk Utility.**

## Step 6: back to Windows
1. Remove the stick and power on. Windows should boot normally.
2. If you suspended BitLocker, it resumes by itself after the reboot count runs out. If you want Secure Boot back on, turn it on in F2 BIOS setup.
3. Plug the stick into Windows. The three `.txt` files are the results.
   - `ioreg-full.txt` contains your generated serials and device details, so **do not post it publicly**.

## Pass / fail
| Check | Pass | If not |
|---|---|---|
| OpenCore picker appears | yes | Secure Boot is still on, or the stick was not booted in UEFI mode |
| Recovery reaches its menu | yes | The photo shows where it stopped: CPU spoof, ACPI, MADT x2APIC or display |
| `hw.logicalcpu` | 24 | CPU topology problem |
| `VMDStatus` in vmdprobe.txt | `scanned` | Decides the internal-disk plan |
| `USB4ProbeStatus` / `NHIStatus` | `done` / `read` | `NHIFirmwareCM` decides how the dock driver is built |
| Windows afterwards | boots unchanged | Remove the stick. Nothing internal was written. |

## Rollback
Remove the stick. That is the whole rollback.
- The config never writes boot entries (`RequestBootVarRouting` on, `LauncherOption` disabled), so Windows Boot Manager stays the firmware default.
- Re-enable Secure Boot if you want it.
