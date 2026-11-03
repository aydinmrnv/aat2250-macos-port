# VMD driver: status and test procedure

| Kext | Purpose | Builds? | Loaded/tested? |
|---|---|---|---|
| `VMDProbe.kext` 0.0.1 | Phase 0: read-only scan of the VMD domain; publishes `VMDTopology` | Yes, x86_64 kext bundle; binary sha256 `bd6b4c42…a4b0e8c` | No |
| `VMDBridge.kext` 0.1.0 | Phase 1 + 2: second PCI root exposing only root-bus SATA AHCI (NVMe and bridges hidden, MSI hidden from IOPCIFamily); level 2 routes the AHCI MSI through VMD MSI-X entry 1 with the ARL004 dummy read | Yes, x86_64 kext bundle; binary sha256 `7a78d810…308591221` | No |

**Build environment**
- Mac Studio, Xcode 27.0, MacOSX27.0 SDK, `-target x86_64-apple-macos26.0`.
- Run `zsh VMDProbe/build.sh` or `zsh VMDBridge/build.sh`. They only use clang, ld, plutil and codesign (ad-hoc).
- Build output is in `~/macos-port-build/vmd-driver/*/build/` on the Mac.

## What is verified vs not
**Verified**
- Both compile with `-Wall -Wextra -Werror`.
- Both link as `Mach-O 64-bit kext bundle x86_64`.
- VMDBridge imports exactly the IOPCIHostBridge symbols listed in DESIGN.md.

**Not verified**
- Whether Tahoe 26.7.2's x86_64 IOPCIFamily exports those symbols. This needs the x86_64 kernel collection, from a KDK or from the installer.
- Whether `sizeof(IOPCIHostBridge)` matches. VMDBridge refuses to load on a mismatch.
- All runtime behaviour.

**Compile shim:** `vmd_sdk_shim.h` exists because the SDK's IOPMAssertionTypes.h ships empty.

## Test ladder
Run each step only after the previous one passes, on the Alienware booted into macOS from a separate disk.
1. **VMDProbe**
   - Boot with `-vmdprobe`, then run `ioreg -l -w0 -r -c VMDProbe` and `log show --last boot --predicate 'eventMessage CONTAINS "VMDProbe"'`.
   - **Pass:** `VMDBusStart` is 0, 128 or 224, and `VMDTopology` lists one class `0x010601` function on the root bus and the NVMe behind a type-1 bridge. **Collect this output.** It decides whether phase 1's root-bus-only policy holds.
2. **VMDBridge level 1** (`-vmdbridge`)
   - **Pass:** a `vmd-domain` nub appears with a host bridge and one IOPCIDevice of class `0x018000`; its `assigned-addresses` fall inside MEMBAR1 (0x72000000+16 MB on this machine).
   - **Also required:** no NVMe or bridge appears under it, and Windows still boots normally afterwards.
3. **VMDBridge level 2** (`vmdbridge=2`, interrupt path written, reviewed and fixed, not run)
   - **Pass:** AppleAHCIPort attaches and both SATA disks are listed by `diskutil list`. Mount the Samsung read-only only (`diskutil mount readOnly`).
   - **Also required:** no AHCI command timeouts in the log over 30 minutes of reads (for example, `dd` of the whole Samsung to /dev/null twice, comparing checksums).
4. **Writes:** only after the D: backup is verified. Erase the Samsung as APFS, run a write and verify soak, then install.

**Kill switch:** `-vmdoff` in the boot-args disables both kexts. Removing the kext from OpenCore Kernel→Add removes it entirely.

## Reviews
**`REVIEW-phase0-1.md`** (independent): no definite panic. Fixes applied:
1. **HIGH:** only the AHCI ABAR's own firmware range is advertised. Before that, the ABAR is sized and must already lie inside MEMBAR1, so it can no longer be relocated onto the hidden NVMe/root-port MMIO.
2. **MEDIUM:** CFGBAR mapping must be ≥ 1 MB.
3. **Power:** the controller and nub join the power tree, and PMstop runs on failure.
4. **MSI:** the hidden-capability length now comes from the MSI control register.

Teardown after a successful start is deliberately unsupported, because the configurator keeps a raw pointer to the bridge. Use `-vmdoff` and reboot.

**Open from that review:**
- The configurator may graft a 64-bit window onto the VMD bridge (PFM64). This needs observation in phase 0/1.
- The ABI size check cannot detect vtable-only changes. Kext linking should catch missing symbols.

**`REVIEW-phase2.md`** (independent). Fixes applied:
1. **HIGH:** an MSI that arrives while the child is disabled is remembered and replayed on enable.
2. **HIGH (confirmed):** disable and unregister wait for in-flight handlers on other CPUs. This fixes a use-after-free.
3. **HIGH:** the hidden MSI capability is re-programmed in a `restoreDeviceState` override, after sleep/wake or D3.
4. **MEDIUM:** level 2 requires a VMD MSI-X table of ≥ 2 entries and ≥ 2 messaged IOKit vectors.
5. **MEDIUM:** an AHCI function with any memory BAR other than the ABAR is refused.
6. **LOW:** MSI per-vector mask bits are cleared.

Areas the review found clean: property resolution, publish ordering, base-class members, MSI layout, and the phase-1 fixes.
