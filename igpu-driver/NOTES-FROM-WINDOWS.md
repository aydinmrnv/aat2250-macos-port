# Notes from the Windows-side Claude (2026-11-04, PST)

Read this before changing anything on the boot stick.

## 1. What broke the boot at ~19:00
It was **not** IGPUScanout. Timeline from the Mac's own DiagnosticReports and the OpenCore logs (RTC = PST+8):
- Boots at 17:35, 17:46 and 17:58 ran IGPUScanout 0.0.3 with `-igpuscanout` and reached the desktop.
- At 18:50 IntelBTPatcher went from 2.5.0 to **2.5.3**. The backup `~/EFI-Backups/EFI-2026-11-04-before-btpatcher-2.5.3` differs from the failing stick **only** in `IntelBTPatcher.kext`.
- The next boot (~19:00) never reached macOS.

**Fix applied:** the stick is now exactly that backup (IntelBTPatcher 2.5.0, IGPUProbe, IGPUScanout, Bluetooth, NullMoth). 2.5.3 is in `EFI/OC/Kexts-backup/IntelBTPatcher-2.5.3.kext`; the config is also `config-working-igpu-bt.plist`.

## 2. Separate open problem: the SATA root disk dropped
`/Library/Logs/DiagnosticReports/Retired/Kernel-2026-11-04-173547.panic`: WindowServer watchdog, "WindowServer failed to spawn with error 5", root disk errors **"Could not recover SATA HDD after 5 attempts"** after 4.6 h of uptime.
- AppleAHCIPort lost the Samsung 860 EVO on the Arrow Lake PCH SATA (8086:7F62). Windows reports the disk healthy.
- Worth checking:
  - `log show --predicate 'sender == "AppleAHCIPort" OR sender == "IOAHCIBlockStorage"' --last 1d`
  - `system_profiler SPSerialATADataType` (link speed, NCQ, TRIM)
  - whether `trimforce` was enabled. Linux blacklists queued TRIM on Samsung 8xx drives.

## 3. Rules for the stick, please
- **One change per boot.** Before each change, copy the whole `EFI` to `~/EFI-Backups/<date>-before-<change>`, as you did for 2.5.3. That backup is what let me find the cause in minutes.
- Don't replace a working kext with a newer build in the same boot as another change.
- The Windows side can read the stick and `~/EFI-Backups` (read-only, via apfs-fuse). Write findings to a file on the stick too.

## 4. iGPU MST (G2), from reading `lgmst.c`
- The stream order matches i915's `mst_stream_enable`:
  - transcoder func → VC payload alloc bit → ACT → `drm_dp_check_act_status` → ALLOCATE_PAYLOAD → TRANSCONF.
- The MSA/PIPEMISC/DDI_FUNC bpc encodings and transport select look right.
- **"Only the very first attempt locked port 2" is the strongest clue.** After a failed attempt the Synaptics hub's downstream ports may stay wedged, and the D3/D0 cycle and POWER_UP_PHY didn't clear it. Before each MST attempt, **cold-reset the dock**: unplug the WD19TB's power and its USB-C for 15 s, then boot. Then run `lgmst --discover` and one `lgmst` attempt, and save the output to the stick.
- If it still doesn't lock from a cold hub, use the Linux i915 capture (README idea). Boot a live Linux with `drm.debug=0x1fe log_buf_len=32M`, then save `dmesg`, `/sys/kernel/debug/dri/*/i915_dp_mst_info`, `i915_display_info`, and DPCD 0x000-0x2FF of the hub plus remote DPCD of both ports. The Windows side can build that stick.
