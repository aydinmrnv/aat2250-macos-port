# Installing macOS Tahoe on the Samsung SSD (AAT2250)

This is the first install attempt. In one boot, it:
- makes the SATA disks visible to macOS through VMDBridge level 2 (`vmdbridge=2`);
- installs macOS on the **Samsung SSD 860 EVO 500GB** (Windows calls it D:).

**What stays untouched:**
- **The Windows NVMe stays hidden.** VMDBridge never shows it to macOS, and OpenCore's `ScanPolicy` (0x370002) skips NVMe devices.
- **Windows' own recovery (WinRE) is on the NVMe.** The Samsung's old Recovery and EFI partitions are leftovers that Windows doesn't use.

> ## ⚠️ Two SATA disks will appear. Erase only the Samsung.
> | Disk | Size | What to do |
> |---|---|---|
> | **Samsung SSD 860 EVO 500GB** | ~500 GB | **erase this one** |
> | **WDC WD60EFZX** | ~6 TB | **never touch.** It holds the data moved off D:. |
> If you are unsure which is which, stop and check the size again.

## Before you start (in Windows)
1. **Back up D:.** Everything on D: must already have a copy elsewhere. On the AAT2250 every folder was compared file-for-file with its copy on E: on 2026-11-04.
2. **Plug in a network cable**, to the PC's Ethernet port or the dock's. Wi-Fi doesn't work. The installer downloads about 15 GB from Apple.
3. **Leave the stick in a rear USB-A port**, and leave the dock connected as usual.

## Step 1: boot Recovery
1. Shut down, power on and tap **F12**. Choose the USB stick.
2. The OpenCore menu now appears for 10 seconds and then picks an entry by itself. **Don't press anything.** Expect 1–3 minutes of loading, then verbose text.
3. Recovery appears. If it freezes for 5 minutes, photograph the screen and stop.

## Step 2: check that the disks appeared
1. Open **Disk Utility**, then **View → Show All Devices**.
2. You should see **Samsung SSD 860 EVO 500GB (~500 GB)**, **WDC WD60EFZX (~6 TB)** and the USB stick.
   - **If the Samsung is missing:** close Disk Utility and open Utilities → Terminal. Run `sh /Volumes/OPENCORE/go`, then Apple menu → Shut Down. Stop there; the results tell us what's wrong.

## Step 3: erase the Samsung (only the Samsung)
1. In the sidebar, click the **top line** of the Samsung, the device itself (~500 GB), not a volume under it.
2. Check again: the window shows **~500 GB** and the Samsung name, not 6 TB.
3. Click **Erase** and set:
   - Name: `Macintosh HD`
   - Format: **APFS**
   - Scheme: **GUID Partition Map**
4. Click Erase, then Done. Close Disk Utility.

## Step 4: install
1. Choose **Reinstall macOS Tahoe**, then Continue, and agree to the licence.
2. Pick **Macintosh HD**, the Samsung.
3. It downloads, then restarts several times. **Leave the stick in and touch nothing.**
   - At each restart the OpenCore menu shows for 10 seconds and should pick **macOS Installer**, later **Macintosh HD**, by itself.
   - If a restart lands back in Recovery, or in Windows, note it and tell me. Nothing is broken.
   - If the screen freezes for more than 10 minutes, take a photo.
4. It ends at the setup assistant (country, account). Set up a local user account. Skipping the Apple ID for now is fine.

## Step 5: save results (in the new macOS)
Open Terminal (Applications → Utilities) and run:

```bash
sudo sh /Volumes/OPENCORE/go
```

Type your new Mac password when asked. Then use Apple menu → Shut Down.

## Back to Windows
Remove the stick, or don't pick it in F12. Windows boots as before, because macOS never touched the NVMe.

## Expected limits right after install
| Area | State |
|---|---|
| Graphics | Slow, not 4K, until the RTX 5090 driver is installed |
| Booting | Only through the USB stick, until OpenCore is moved to the Samsung's EFI partition (needs approval) |
| Network | Ethernet works; Wi-Fi and Bluetooth don't |
| Sound | Through the dock's USB audio only |
| Sleep | Off. Use Shut Down. |
