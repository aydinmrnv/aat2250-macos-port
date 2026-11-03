# Review: VMDProbe (phase 0) and VMDBridge (phase 1)

Reviewer: independent read-only review, 2026-11-02. Nothing was built or run. Apple behaviour is taken from IOPCIFamily-726.100.6 in `src/vmd-refs/iopci`. That version may not match the IOPCIFamily inside the target macOS.

Verdicts:
- **CONFIRMED** means the code does this.
- **PLAUSIBLE** means it depends on runtime state or on xnu code that isn't in the reference tree.
- **NOT-AN-ISSUE** means the concern was checked and holds up.

## Overall verdict

- **Phase 0 (VMDProbe)** keeps its read-only promise. I found no path that writes config space or MMIO, and no out-of-bounds access.
- **Phase 1 (VMDBridge):**
  - The config accessors are memory-safe. Only the root bus is reachable, and the NVMe can't be enumerated or written through config space.
  - I found no definite panic in `addHostBridge` or the start path. This assumes the ABI really matches.
  - The main open risk is a **policy** risk, not a crash: IOPCIConfigurator gets the whole of MEMBAR1 with no knowledge of the hidden devices' windows. If it ever relocates the AHCI ABAR, the ABAR could land on top of the NVMe's or root port's MMIO (finding 1).
  - Fix findings 1 and 2 before the first load. The rest are hardening, or matter only for phase 2.

---

## Findings, ranked by severity

### 1. HIGH (safety policy): the configurator may place the AHCI ABAR on top of hidden NVMe or root-port MMIO. PLAUSIBLE
- **Where:** `VMDBridge.cpp:114` (`addBridgeMemoryRange(fMem1, fMem1Len)`), together with `IOPCIConfigurator.cpp:3703-3711` and `:3908/3935`.
- **What happens:**
  - The host bridge advertises all 16 MB of MEMBAR1 as free.
  - The hidden root port's memory window, and possibly the NVMe BAR, live inside MEMBAR1. IOPCIFamily never sees them, so it can't reserve them.
  - The newly found AHCI function is not `kPCIDeviceStateAttached`, and this pass runs with `kIOPCIConfiguratorBoot` set, so its ranges are marked relocatable.
- **Failure scenario:** the ABAR moves if any of these hold:
  - the firmware ABAR is not inside `[MEMBAR1, MEMBAR1+16MB)` (DESIGN risk 3 is still unverified);
  - the size probe reads back oddly;
  - the allocator prefers another spot.

  Once moved, the ABAR can overlap the root port's window. AHCI MMIO (HBA reset, PxCMD) is then decoded by the root port and forwarded to the **NVMe's registers**. That breaks the "never writable" rule and could corrupt the Windows disk's controller state.
- **Fix:** before creating the bridge, read each allowed function's memory BARs, require them to be non-zero and fully inside MEMBAR1, and refuse to start otherwise. Then advertise only the ABAR's own aligned range, for example `addBridgeMemoryRange(abarBase, abarSize)`, not the whole of MEMBAR1. That way any relocation can only land on the ABAR itself.

### 2. MEDIUM (memory safety): no check of the CFGBAR mapping length. CONFIRMED
- **Where:** `VMDBridge.cpp:322-327` and `:137-141`.
- **The gap:** `findAllowed` and every accessor index up to `ecam + (31<<15) + (7<<12) + 0xFFF` = 1 MB − 1. `fCfgMap->getLength()` is never checked. VMDProbe does check it (line 151).
- **When it bites:** on this machine the CFGBAR is 256 MB, so it's fine today. If the BAR is unsized, or the firmware/OS ever gives a smaller one, the very first scan reads past the mapping, which page-faults and panics.
- **Fix:** add `if (fCfgMap->getLength() < (1u << 20)) fail`.
- **Otherwise the math is sound:** bus is always `fBus`, so the ECAM index is 0. dev ≤ 31, fn ≤ 7, and reg < 0x1000 because `registerNumExtended` is 4 bits.

### 3. MEDIUM (resource placement): a PFM64 window may be added to the VMD bridge. PLAUSIBLE
- **Where:** `IOPCIConfigurator.cpp:684-699`.
- **What happens:**
  - The VMD bridge reports only a 32-bit memory range, so `hasHost64` is false.
  - If PC00's ACPI `_CRS` already had a 64-bit window, `fAddedHost64` is still false. That is likely, because the VMD's own BARs sit at `0xB8_xxxx_xxxx`.
  - The configurator then grafts `[2^physbits − fPFM64Size, 2^physbits)` onto **the VMD bridge** and sets `fAddedHost64 = true` for the whole system.
- **Impact:** the VMD doesn't decode that range. It can also overlap PC00's real 64-bit window. The AHCI ABAR is 32-bit, so nothing should be placed there today, but it's a hidden coupling.
- **Fix:** after `addHostBridge`, log or inspect the bridge's ranges with `pci=0x...` logging on the first boot. Finding 1's fix (advertise only the ABAR) doesn't prevent this, so just verify it.

### 4. MEDIUM (phase 2 hazard): `IOSimpleLock` isn't interrupt-safe. PLAUSIBLE (future)
- **Where:** `VMDBridge.cpp:176-219`.
- **Phase 1:** fine. Accesses come from the configurator work loop and driver threads, and a spinlock around one MMIO access is acceptable.
- **Phase 2:** DESIGN's ARL004 workaround does a dummy config read *inside the VMD interrupt handler*. If that read goes through these accessors while a thread on the same CPU holds `fLock`, the CPU deadlocks, because `IOSimpleLockLock` doesn't mask interrupts.
- **Fix:** either use `IOSimpleLockLockDisableInterrupt` / `UnlockEnableInterrupt`, or drop the lock. A single aligned MMIO access needs no lock; Linux's `cfg_lock` only serialises read-modify-write.

### 5. MEDIUM (lifecycle): the host bridge is never added to the power tree explicitly. PLAUSIBLE
- **Where:** `VMDBridge.cpp:333-353`; `IOPCIBridge.cpp:1351-1358`.
- **What happens:** `IOPCIBridge::start` calls `PMinit`, `registerPowerDriver`, then `provider->joinPMtree(this)`. The provider is `VMDDomainNub`, which never calls `PMinit`, and `VMDController` doesn't join the power tree either.
- **The open question:** whether the bridge ends up a power child of the VMD IOPCIDevice depends on how xnu's `IOPlatformExpert::PMRegisterDevice` walks providers, and that code isn't in the reference tree.
- **If it ends up parentless:** the bridge may never reach `kIOPCIDeviceOnState`. Children would then stay in `needsProbe` (`IOPCIBridge.cpp:1183-1184`) or be idled to D3. That's a functional hang (no AHCI nub or no match), not a panic.
- **Fix:** in `VMDController::start`, call `PMinit(); provider->joinPMtree(this); registerPowerDriver(...)` with a single always-on state. Do the same for the nub, so the chain is VMD IOPCIDevice → controller → nub → bridge.

### 6. LOW-MEDIUM (lifecycle): no `stop()` or `free()`, and a leaked lock. CONFIRMED (benign by accident)
- **Where:** `VMDController` (no stop/free), and `VMDHostBridge` (no free; `fLock` from `setDomain` is never freed, including on the fail path at `:344-346`).
- **Retain/release:**
  - On success, the controller keeps a +1 on `fBridge`, `fNub` and `fCfgMap` forever.
  - Attach adds its own retains, and the fail path balances them correctly: it detaches only after a successful attach, then calls `OSSafeReleaseNULL`.
  - Releasing a bridge whose `init` failed is safe, because `reserved` is null-checked and `_publishNotifier` is zeroed.
- **Why the leak matters:** IOPCIBridge has no way to remove a host bridge from the configurator. `IOPCIConfigEntry::hostBridge` is a raw pointer, and every later `configure()` pass (Thunderbolt, hotplug) walks all roots and calls `hostBridge->configRead*`. The leak keeps both the bridge and the ECAM mapping alive, which prevents a use-after-free.
- **Fix:** keep it that way deliberately. Add a comment, and make `stop()` refuse, or never release `fCfgMap` or the bridge. Free `fLock` in `VMDHostBridge::free()`. Do **not** add a "clean" teardown that unmaps the CFGBAR.

### 7. LOW: MSI hidden length is a fixed 24 bytes. CONFIRMED (minor)
- **Where:** `VMDBridge.cpp:268`.
- **What happens:** a 32-bit MSI without masking is 10 bytes, so a 24-byte block can swallow the start of a following capability. Writes to it (for example PMCSR or PCIe DevCtl) would be silently dropped.
- **Safety:** this errs on the safe side and never exposes MSI.
- **Fix:** derive the length from MSI Message Control bits 7 and 8: 10, 14, 20 or 24 bytes.

### 8. LOW: the MSI/MSI-X hiding has a capacity fallback. NOT-AN-ISSUE in practice
- **Where:** `VMDBridge.cpp:266`.
- **What happens:** once `hiddenCount == 4`, any further MSI or MSI-X capability goes into the *visible* list. Real functions have at most one of each, so this is unreachable.
- **Fix:** for defence in depth, refuse the function instead, so the code fails closed.

### 9. Capability-chain virtualization. NOT-AN-ISSUE (verified)
- **`buildCapView`:**
  - `ptr` is masked to `0xFC` and bounded to `[0x40, 0x100)`; `visible[48]` is guarded by `n < 48`.
  - `nextVisible[visible[i]/4]` has index ≤ 63 against an array of 64.
  - `0xFF` is a valid end sentinel, because real pointers are dword-aligned.
- **`virtualRead32`:** the bounds test `reg < 0x100` comes before indexing. 8- and 16-bit reads go through `configRead32`, so 0x34 and every visible next-byte are rewritten for every width.
- **Hidden capabilities:**
  - They are unreachable through the chain, but a raw read of their offset still returns real data, which is harmless.
  - Writes are blocked by `writeAllowed` overlap tests for every width, including `configWrite8` on MSI-X Control (cap+3).
  - IOPCIFamily's own MSI save/restore keys off `reserved->msiCapability`, which stays 0.
- **Extended capabilities (≥ 0x100):** they pass through unvirtualized. That's fine, because MSI and MSI-X are never extended capabilities. AER and LTR writes are allowed and affect only the AHCI function.
- **Duplicate pointers or loops:** these can only re-create the hardware's own loop, and the walk is still bounded.
- **Interrupt Pin:** `writeAllowed` blocks any write covering 0x3D, so 16- and 32-bit writes at 0x3C are dropped (Interrupt Line is lost, which is harmless). An 8-bit write to 0x3C is allowed.

### 10. Accessor offset composition and alignment. NOT-AN-ISSUE (minor nit)
- **Offset composition:** `IOPCIConfigurator.cpp:4684` etc. set `es.registerNumExtended = offset >> 8` and pass the low byte as `UInt8`. `(ext << 8) | offset` therefore rebuilds the full offset, and with the 4-bit field it is always < 0x1000, so the `reg >= 0x1000` checks are dead but harmless.
- **Alignment:** 32-bit accesses use `& 0xFC`, 16-bit `& 0xFE`, 8-bit is exact, so there are no misaligned MMIO accesses.
- **Nit:** a misaligned 16-bit *read* at offset 3 returns bytes 2-3, and a misaligned 16-bit write is silently realigned. No IOPCIFamily caller does either.
- **Bus filter:** `lookup()` rejects every bus other than `fBus`, so bridges, and the NVMe behind its root port, can never be reached or written.

### 11. IOPCIConfigurator / IOPCIBridge integration: no null dereference found. NOT-AN-ISSUE
- **Provider:** `bridge->dtNub = getProvider()` is the nub. It is not an IOPCIDevice, so the panic at `:614` can't trigger.
- **`IOPCICopyACPIDevice(nub)`:** it is null-safe and returns null, so `removeFixedRanges` is skipped, and the DT/ACPI plane walks at `:1251-1258` are skipped or empty.
- **`fRootVendorProduct`** is overwritten with `0xFFFFFFFF` by the bus-0, dev-0 probe at `:631`. It is used only for the PFM64 quirk test, so this is harmless.
- **`firstBusNum == lastBusNum`** gives a one-bus range of size 1, which is valid.
- **`findEntry` / `findEntryByAddress`** stop at the nub because it is neither an IOPCIDevice nor an IOPCIBridge. They then compare `hostBridge == VMDHostBridge`, so the VMD bus number overlapping PC00's bus numbers doesn't matter.
- **`ioDeviceMemory() == nullptr`** is guarded at `IOPCIBridge.cpp:3073` and `:4265`. The `initSubRange(ioDeviceMemory())` at `:4245` is reached only for a subrange that was created from a non-null I/O memory, so it is never reached here. Any I/O BARs simply fail to allocate.
- **`addBridgeMemoryRange`:** `IOPhysicalAddress` is 64-bit on x86_64 and MEMBAR1 is below 4 GB anyway. Note that it also calls `AppleVTD::addMemoryRange`, which can't be undone, even on the fail path.
- **`IOPCIHostBridge::probe` on x86 (ACPI_SUPPORT)** shares `gBridgeData` (retained, and released in `free`) and sets `_domainId = 0`. It **ignores** the nub's `domain-id`.
  - `IOPCIBridge::start` does read `domain-id` into `reserved->domainId = 0x100`. That value is used only by `pci_log`, where `1 << 0x100` is undefined behaviour (x86 masks it to `1 << 0`).
  - The property therefore does **not** give a distinct domain. Use a value below 32, or drop it.
- **Late `configure(kIOPCIConfiguratorBoot)` over the whole tree:**
  - Already-attached static devices are skipped (`:3712-3715`), and `bootResetProc` only touches hotplug bridges without sub-ranges.
  - `setConsoleInfo(kPEDisableScreen/Enable)` toggles the console. That's cosmetic.
  - Its interaction with Apple's real roots looks safe, but verify it with `pci=` logging on the first load.
- **Calling `probe`/`start` by hand, outside matching:** acceptable. `probe` is called, so `bridgeData` is not null in `start`. `registerService` happens inside `IOPCIBridge::start`. Note that `start` runs `configOp` and `probeBus` synchronously on the shared configurator work loop from the matching thread. That's not a deadlock, because the VMD nub's matching isn't run inside that gate.

### 12. ABI check. PLAUSIBLE gap, mitigated
- **Where:** `VMDBridge.cpp:246-253`.
- **What it catches:** `getClassSize()` against `sizeof(mirror)` catches ivar size drift in both IOPCIBridge (as the SDK header sees it) and IOPCIHostBridge.
- **What it misses:** reordered ivars of equal size, and changes to the vtable layout.
- **Why that's mostly covered:** the mirror adds no new virtuals, only overrides. Kext linking (kmutil/kxld vtable patching) resolves overrides by symbol and refuses to link on a mismatch, so a vtable skew should fail at load rather than panic.
- **Shim typedefs:** the `vmd_sdk_shim.h` typedefs match xnu's widths: `IOPMDriverAssertionID` and `Type` are `uint64_t`, `Level` is a 32-bit enum. They appear only in IOPCI2PCIBridge, which is never instantiated.
- **Fix:** keep the size check, and also compare `IOPCIBridge::metaClass->getClassSize()` against `sizeof(IOPCIBridge)`. Confirm that IOPCIFamily actually exports `IOPCIHostBridge::gMetaClass` and its vtable, otherwise the kext won't link.

### 13. Kill switch and boot-args. NOT-AN-ISSUE
- `-vmdoff` wins in both kexts, both in `probe` and in `start`.
- `vmdbridge=N` maps to 1 or 2, and any other value gives 0. A bare `vmdbridge` parses as boolean 1, and `-vmdbridge` gives 1.
- The xnu parser includes the leading `-` in the name, so the `vmdbridge` and `-vmdbridge` names don't collide.
- Nothing loads without an opt-in.
- VMDProbe and VMDBridge compete for the same nub in the default match category, so only one attaches.

### 14. Phase 0 (VMDProbe) read-only guarantee. NOT-AN-ISSUE (one LOW note)
- **Read-only:** I found no config writes. The CFGBAR is mapped `kIOMapReadOnly`, the bus count is bounded by `size>>20` and `256-busStart`, and the mapping is released at the end.
- **Bounds:** the capability walk is bounded at 48. `ptr` is masked to `0xFC` and is below 0x100, so the 4-byte reads stay inside the 4 KB function window.
- **LOW, PLAUSIBLE:** a full scan of every bus (up to 256×32 function-0 reads) depends on the VMD returning UR quickly for empty or link-down buses. Any completion timeouts would multiply into a long boot stall. Consider scanning only the root bus plus the secondary/subordinate ranges of bridges that are found, as Linux does.

### 15. Phase 2 notes (not phase-1 bugs). PLAUSIBLE
- **Function 0 hidden:** if an allowed AHCI function is ever at function > 0 while function 0 is hidden, the configurator skips the whole device, because function 0 reads as absent. Intel SATA is at 17.0, so it's fine here.
- **DMA requester ID:** DMA from VMD children arrives with the **VMD's** requester ID. `AppleVTD::adjustDevice(nub)` keys off the child's own segment-0 BDF. With VT-d enabled and per-device spaces, AHCI DMA (FIS receive starts even without interrupts) could fault or use the wrong page tables. Check this before running level 2 with AppleAHCIPort.
