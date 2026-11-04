# Installing macOS Tahoe on the Samsung SSD (AAT2250)

**Intel VMD is switched off in the BIOS** (SATA/NVMe Operation = AHCI/NVMe, 2026-11-04). macOS sees the SATA disks with Apple's own AHCI driver, so the VMD drivers are disabled in the config. To switch it the safe way, Windows was booted once in Safe Mode after the change (`bcdedit /set "{current}" safeboot minimal`, then `bcdedit /deletevalue "{current}" safeboot`). In PowerShell, keep the quotes around `{current}`.

This installs macOS on the **Samsung SSD 860 EVO 500GB** (Windows' D:).

**What stays untouched:**
- **macOS will also see the Windows NVMe (SK hynix 2 TB).** Don't touch it. OpenCore's ScanPolicy never offers it as a boot entry.
- **Windows' own recovery (WinRE) is on the NVMe.** The Samsung's old Recovery and EFI partitions are leftovers that Windows doesn't use.

> ## ⚠️ Two SATA disks will appear. Erase only the Samsung.
> | Disk | Size | What to do |
> |---|---|---|
> | **Samsung SSD 860 EVO 500GB** | ~500 GB | **erase this one** |
> | **WDC WD60EFZX** | ~6 TB | **never touch.** It holds the data moved off D:. |
> | **SK hynix 2048GB** (NVMe) | ~2 TB | **never touch.** It's Windows. |
> If you are unsure which is which, stop and check the size again.

## Before you start (in Windows)
1. **Back up D:.** Everything on D: must already have a copy elsewhere. On the AAT2250 every folder was compared file-for-file with its copy on E: on 2026-11-04.
2. **Plug in a network cable**, to the PC's Ethernet port or the dock's. Wi-Fi doesn't work. The installer downloads about 15 GB from Apple.
3. **Leave the stick in a rear USB-A port**, and leave the dock connected as usual.

## Step 1: boot Recovery
1. Shut down, power on and tap **F12**. Choose the USB stick.
2. The stick starts in **recovery mode**: OpenCore looks only at USB, shows no menu and boots the Recovery image by itself. **Don't press anything.** Expect 1–3 minutes of loading, then verbose text.
   - If you see an OpenCore menu with **Windows** in it, the stick is still in install mode from an earlier attempt. Don't let it start Windows through OpenCore: power off, boot Windows normally (stick out), and tell me.
3. Recovery appears. If it freezes for 5 minutes, photograph the screen and stop.

## Step 2: check that the disks appeared
1. Open **Disk Utility**, then **View → Show All Devices**.
2. You should see **Samsung SSD 860 EVO 500GB (~500 GB)**, **WDC WD60EFZX (~6 TB)**, the **SK hynix NVMe (~2 TB, Windows)** and the USB stick.
   - **If the Samsung is missing:** close Disk Utility and open Utilities → Terminal. Run `sh /Volumes/OPENCORE/go`, then Apple menu → Shut Down. Stop there; the results tell us what's wrong.

## Step 3: erase the Samsung (only the Samsung)
1. In the sidebar, click the **top line** of the Samsung, the device itself (~500 GB), not a volume under it.
2. Check again: the window shows **~500 GB** and the Samsung name, not 6 TB.
3. Click **Erase** and set:
   - Name: `Macintosh HD`
   - Format: **APFS**
   - Scheme: **GUID Partition Map**
4. Click Erase, then Done. Close Disk Utility.

## Step 3b: switch the stick to install mode
Erasing the Samsung removed its old Windows boot files, which are what put **Windows** in the OpenCore menu. Now let OpenCore look at SATA disks too, so it can start the installer from the Samsung:
1. Utilities → **Terminal**, and type:

   ```bash
   sh /Volumes/OPENCORE/installmode
   ```
2. Wait for **INSTALL MODE ON**, then close Terminal.

(`sh /Volumes/OPENCORE/recoverymode` switches back.)

## Step 4: install
1. Choose **Reinstall macOS Tahoe**, then Continue, and agree to the licence.
2. Pick **Macintosh HD**, the Samsung.
3. It downloads, then restarts several times. **Leave the stick in.**
   - **With USB first in the BIOS boot order,** every restart comes back to the stick by itself. Otherwise, tap F12 and choose the USB stick at each restart.
   - **If a restart switches the PC off instead,** power it on (F12 if needed). This config carries a restart fix (FADT reset register → 0xCF9), but it isn't tested yet.
   - The OpenCore menu shows for 10 seconds and should pick **macOS Installer**, later **Macintosh HD**, by itself. **Never let it start Windows from this menu.** If Windows is the highlighted entry, power off before the 10 seconds run out and tell me.
   - If it lands back in Recovery instead, tell me.
   - If the screen freezes for more than 10 minutes, take a photo.
4. It ends at the setup assistant (country, account). Set up a local user account. Skipping the Apple ID for now is fine.

## Step 5: save results (in the new macOS)
Open Terminal (Applications → Utilities) and run:

```bash
sudo sh /Volumes/OPENCORE/go
```

Type your new Mac password when asked. Then use Apple menu → Shut Down.

## Back to Windows
**Remove the stick** and power on. Windows boots as before, because macOS never touched the NVMe. With USB first in the BIOS boot order, the stick always wins while it's plugged in. Never start Windows from the OpenCore menu: OpenCore's macOS ACPI changes would apply to Windows too.

## Expected limits right after install
| Area | State |
|---|---|
| Graphics | Slow, not 4K, until the RTX 5090 driver is installed |
| Booting | Only through the USB stick, until OpenCore is moved to the Samsung's EFI partition (needs approval) |
| Network | Ethernet works; Wi-Fi and Bluetooth don't |
| Sound | Through the dock's USB audio only |
| Sleep | Off. Use Shut Down. |
