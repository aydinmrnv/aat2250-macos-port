# Separate-drive install, recovery and rollback plan (A2)

Scope: an isolated macOS experiment on a **new, dedicated external USB SSD**. This is the only storage path that works without touching VMD; see platform-analysis §3.

This is a plan. Nothing in it has been executed. Every step marked **USER** is done by the user personally. No agent reads recovery keys, serials or UUIDs.

## Invariants (must hold at every step)
1. Windows Boot Manager stays the firmware default. Nobody edits the Windows ESP (disk 0 p1) or the Samsung ESP (disk 2).
2. OpenCore is configured not to touch the firmware boot list: `LauncherOption=Disabled`, `AllowSetDefault=false`, `RequestBootVarRouting=true`, `ResetNvramEntry` disabled. macOS is reached only through the one-time F12 boot menu → the USB SSD.
3. OpenCore never boots Windows (`ScanPolicy 0x00200303` hides internal and NTFS volumes).
4. No BIOS storage-mode (VMD/RAID/AHCI) change in this plan, ever.
5. VT-d, TPM, virtualization and VBS stay as they are.

## Gate 0: prerequisites (all must be YES; any NO = STOP)

| # | Check | Who / how |
|---|---|---|
| G0.1 | Full image or file backup of disk 0 (Windows), disk 1 (E:, including the WSL vhdx on E:\WSL2-Distros) and disk 2 (D:), stored **off this PC**, with one test restore of a few files | USER |
| G0.2 | BitLocker status confirmed with admin `manage-bde -status`. If any volume is protected, the user confirms they can see the recovery key *outside this PC* (MS account / printout). An agent never reads the key. | USER |
| G0.3 | Windows recovery USB (Media Creation Tool) made and tested to boot | USER |
| G0.4 | BIOS settings photographed (Boot, Secure Boot, Storage, Virtualization, Power pages) as the rollback reference. Dell BIOS 1.20.0 version noted. | USER |
| G0.5 | A new blank SSD (≥256 GB) plus a USB 3.2 Gen2 enclosure, never used for data | USER purchase |
| G0.6 | A separate installer USB stick (≥16 GB) built from Apple's recovery via OpenCore `macrecovery` (official 1.0.8 `Utilities/macrecovery`) | USER |
| G0.7 | Secure Boot decision taken from platform-analysis §4, with consequences accepted in writing in PROGRESS-LOG | USER |
| G0.8 | Config draft completed: zero `UNRESOLVED-` strings, SMBIOS generated locally, every kext/driver pinned by SHA-256, ocvalidate 1.0.8 says "No issues found", ACPI SSDTs built from this machine's dump | Agents + USER |
| G0.9 | Every monitor for the macOS test cabled to the RTX 5090 (not the iGPU) | USER |

## Phase 1: isolate (physical)
1. Shut down fully and remove mains power.
2. **Disconnect the data cables (or power) of the WD 6 TB and Samsung 860 SATA drives.** They are easy to reach in bays.
3. Disconnecting the M.2 Windows NVMe means removing the drive. Do this only if the user is comfortable with it (ESD, M.2 screw). Leaving it installed is acceptable: it sits behind VMD, so neither OpenCore's ScanPolicy nor macOS can see it.
4. Disconnect the WD19TB dock and all other USB storage.
5. Gate G1: power on, enter BIOS (F2). Confirm only the Windows NVMe (if left installed) is listed. Change nothing. Exit.

## Phase 2: install
1. If option A/B was chosen, set firmware Secure Boot as decided in G0.7 now. Before doing so, suspend BitLocker for one reboot if it is on (USER, admin).
2. Plug in the installer stick and the target SSD on **rear motherboard USB ports**. F12 → choose the installer stick's UEFI entry.
3. In Disk Utility, **verify the target by model, capacity and bus=USB** before erasing. Erase it as GUID/APFS. If any disk with an unexpected size is listed, STOP.
4. Install macOS Sequoia 15.x (exact build to be agreed with A1's driver requirements).
5. Copy the validated EFI to the **target SSD's own ESP**, never an internal one. Remove the installer stick.

## Phase 3: acceptance tests (record pass/fail in PROGRESS-LOG; a failure means stop, not workaround)

| ID | Test | Pass criterion |
|---|---|---|
| T1 | Cold boot via F12 → USB SSD → OpenCore → macOS, 5 times | 5/5 reach the desktop. No kernel panic in `/Library/Logs/DiagnosticReports`. |
| T2 | CPU | `sysctl hw.physicalcpu hw.logicalcpu` = 24/24 (or 8/24 with CTR). `machdep.cpu` model as spoofed. Clock drift <1 s/hour against NTP. |
| T3 | Power | Idle package power/frequency drop (Intel Power Gadget is unavailable, so use `powermetrics`). No sustained max clocks at idle. Temperatures within Windows norms. |
| T4 | Restart and shutdown from macOS, 5 times each | Clean power-off and restart; the next F12 boot works |
| T5 | Sleep/wake | Expected FAIL: the firmware has no S3. Record the behaviour. If it fails, set `pmset -a sleep 0 disksleep 0 hibernatemode 0` and treat sleep as unsupported. Tests are 10 cycles if attempted. |
| T6 | Recovery | Boot macOS Recovery from the OpenCore picker (APFS Recovery volume). Reaches Recovery and Disk Utility. |
| T7 | Windows untouched | Remove the USB SSD, power on with no F12. Windows Boot Manager boots Windows directly. BitLocker shows no recovery prompt (or the expected one, if Secure Boot was changed). `Confirm-SecureBootUEFI` / msinfo32 state matches the G0.4 photos or the documented change. WSL2 starts. VBS/HVCI is still running (msinfo32). |
| T8 | Firmware boot list | BIOS boot sequence: Windows Boot Manager is still first and no new "OpenCore" entry was added |
| T9 | Driver gates (A1/A3) | Only after T1-T8 pass |

## Phase 4: reconnect
After T1-T8 pass, power off and reconnect the SATA drives. Repeat T7. In macOS, confirm the internal disks remain **invisible** (expected, because of VMD). Do not install NTFS write drivers. Never mount Windows volumes read-write.

## Rollback (any failure or user decision)
1. Power off. **Unplug the USB SSD.** This alone takes macOS and OpenCore out of the boot path.
2. If Secure Boot was changed, restore it to the G0.4 state (photo). If BitLocker prompts, enter the recovery key (USER).
3. If anything else in the BIOS was changed, restore it from the photos. If the BIOS is unrecoverable, use Dell "Restore Settings → BIOS defaults". **Warning:** BIOS defaults may differ from the current settings, so compare against the photos afterwards.
4. Boot Windows. Verify T7, then reconnect drives if they were removed.
5. If Windows does not boot: boot the Windows recovery USB → Startup Repair. Do not run `bcdboot`/`diskpart` against the ESP without a separate plan. Escalate rather than improvise.
6. The macOS SSD can be wiped later in any machine. It holds no Windows data.

## No-go conditions (abort the whole experiment)
- Any step would require a VMD/RAID/AHCI change, an internal ESP write, or shrinking any partition.
- A BitLocker recovery key cannot be produced by the user.
- The target SSD cannot be unambiguously identified.
- Unexplained Windows changes appear after any phase (boot order, Secure Boot state, BitLocker state, VBS).
