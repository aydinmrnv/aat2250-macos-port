# Booting the test USB on the Alienware Area-51 AAT2250

**What this boot is:** a **diagnostic boot**, not an install.
- It starts the macOS Tahoe recovery environment from a USB stick.
- It saves a hardware report to the stick (step 5).
- **Nothing is written to any internal disk.** Windows stays the default boot.

**Status (2026-11-04): this config boots macOS Tahoe Recovery on the AAT2250.** It took ten test boots; the progress log
(`deliverables/PROGRESS-LOG.md`) has each failure and its fix.

**Current test: VMDProbe v0.0.3 + VMDBridge v0.1.2 level 1.** Boot 12 found why nothing was seen behind VMD: macOS
programmed only the low half of the VMD's 64-bit address registers, so the VMD answered at a different address than
the one macOS read. VMDBridge now corrects that first. All internal disks sit behind Intel VMD, which macOS cannot see
by itself.
- **VMDProbe** (read-only) scans the whole VMD config window and counts what the reads return.
- **VMDBridge level 1** (`-vmdbridge`) tries to expose the SATA controller behind VMD as a second PCI root, with its
  class disguised so no storage driver attaches (no disk reads or writes). It refuses and records why if anything is
  unexpected. The Windows NVMe stays hidden by design.

USB4Probe stays off. If this boot freezes before Recovery, photograph the bottom of the screen.
Also new in this config: `SSDT-FIXSHUTDOWN-AAT2250`, so Shut Down should power off instead of restarting.

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

   ```bash
   cp tools/go /Volumes/OPENCORE/go
   ```
5. Eject:

   ```bash
   diskutil eject OPENCORE
   ```

The stick should now hold `EFI/`, `com.apple.recovery.boot/` and `go` at its top level.

## Step 4: boot it (on the Alienware)
1. Shut down. Leave the **WD19TB dock connected** the way it normally is: its state is part of the test.
2. Plug the stick into a **rear motherboard USB-A port**, not the dock and not a front port.
3. Power on and tap **F12**. Choose the USB stick, listed as **UEFI: <stick name>**.
4. **Touch nothing.** There is no picker (`ShowPicker false`): OpenCore starts the recovery image by itself. On a USB 2.0
   stick, loading and verifying the 1 GB image takes **1–3 minutes with no visible progress**.
5. Verbose text scrolls, then the Recovery screen (language, then Utilities) appears.
   - **If the text stops for 5 minutes,** photograph the bottom of the screen. That photo is the test result. Go to step 6.
6. Note **which monitors show anything at all**: the S3225QS on the 5090, and the two LGs on the iGPU.

**What the display looks like in Recovery (expected for now):**
- **Laggy.** There is no driver yet for the RTX 5090 or the Intel iGPU, so macOS draws everything with the CPU onto
  the plain firmware framebuffer, with no acceleration.
- **Not 4K, soft.** OpenCore asks the RTX 5090's UEFI firmware (GOP) for its highest mode (`Resolution Max`). NVIDIA's
  firmware usually offers less than the monitor's native 4K, macOS shows that mode unscaled, and the monitor stretches it.
  Forcing 3840x2160 (`ForceResolution`) is known to give a black screen on NVIDIA firmware, so it is not set.
  The real fix for both is the RTX 5090 driver (native resolution, 120 Hz, acceleration): a later phase.

## Step 5: collect results (in macOS Recovery)
1. If a language screen appears, pick one. Then choose **Utilities → Terminal** from the menu bar.
2. Type this and press Return (the script is on the stick; it takes about 30 seconds):

   ```bash
   sh /Volumes/OPENCORE/go
   ```

   It saves the VMD drivers' results, the full device tree, the loaded drivers, the disks macOS can see, the CPU
   info, the kernel log, then runs `sync` and lists the files. Wait for **DONE**.
   The `.txt` files should each be above 0 bytes (`ioreg-full.txt` is several MB).
3. **Shut down from the Apple menu → Shut Down.** Do not use the power button: the stick is FAT32, and writes that
   were not flushed are lost when the power is cut (this lost the first run's files). **Do not open "Install macOS" or
   Disk Utility.**
   - With the shutdown fix, the PC should power off. If it restarts instead, note that; it tests `SSDT-FIXSHUTDOWN`.

## Step 6: back to Windows
1. Remove the stick and power on. Windows should boot normally.
2. If you suspended BitLocker, it resumes by itself after the reboot count runs out. If you want Secure Boot back on, turn it on in F2 BIOS setup.
3. Plug the stick into Windows. The `.txt` files are the results.
   - `ioreg-full.txt` contains your generated serials and device details, so **do not post it publicly**.

## Pass / fail
| Check | Pass | If not |
|---|---|---|
| OpenCore starts (log file appears on the stick) | yes | Secure Boot is still on, or the stick was not booted in UEFI mode |
| Recovery reaches its menu | yes (since 2026-11-04) | The photo shows where it stopped; compare with the progress log |
| Keyboard and mouse work in Recovery | yes (since 2026-11-04) | Use a rear USB-A port |
| `hw.logicalcpu` in sysctl.txt | 24 | CPU topology problem |
| `VMDStatus` in vmdprobe.txt (probes on) | `scanned` | Decides the internal-disk plan |
| `USB4ProbeStatus` / `NHIStatus` (probes on) | `done` / `read` | `NHIFirmwareCM` decides how the dock driver is built |
| Windows afterwards | boots unchanged | Remove the stick. Nothing internal was written. |

## Rollback
Remove the stick. That is the whole rollback.
- The config never writes boot entries (`RequestBootVarRouting` on, `LauncherOption` disabled), so Windows Boot Manager stays the firmware default.
- Re-enable Secure Boot if you want it.
