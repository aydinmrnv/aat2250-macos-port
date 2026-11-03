# Review: VMDBridge phase 2 (child interrupts) and phase-1 fixes

**Scope:**
- `VMDBridge/VMDInterrupts.h` and `VMDBridge/VMDInterrupts.cpp`
- `VMDBridge/VMDBridge.cpp` (`publishNub`, the level-2 part of `start`, and the phase-1 fixes)

**Checked against:** IOPCIFamily-726.100.6 (`IOPCIDevice.cpp`, `IOPCIBridge.cpp`, `IOPCIMessagedInterruptController.cpp`, `IOPCIPrivate.h`) and the FreeBSD/Linux `vmd.c` in `src/vmd-refs`.

**Not available locally:** the xnu sources for IOService, IOInterruptController and IOPlatformExpert. Statements about xnu below come from general knowledge of those files and are marked where I'm unsure.

## Overall verdict

The routing design is sound, and the IOPCIFamily integration points work:
- the timing of `publishNub`;
- the lazy `getProperty` resolution;
- the hidden MSI capability.

A driver's `registerInterrupt()` on the AHCI nub will reach `VMDInterruptController`.

The weak part is the controller's runtime semantics:
- **Lost interrupts:** an MSI that arrives while the child is disabled is dropped.
- **Teardown race:** disable and unregister don't wait for a handler that is already running.
- **Sleep/wake:** nothing re-programs the child's MSI after a power transition.

All three are functional hangs or use-after-free on teardown, not routing errors. Fix 1–3 before running AppleAHCIPort on level 2 for any length of time.

---

## Findings, ranked by severity

### 1. HIGH: an MSI that arrives while the child is disabled is lost for good. PLAUSIBLE (likely to bite)
- **Where:** `VMDInterrupts.cpp:155-160` (`disableInterrupt` just clears `enabled`) and `:177` (`if (!c.enabled || !c.handler) continue;`).
- **Why it's lost:** MSI is edge-signalled. AHCI sends a new message only when interrupt status goes from clear to set. If `IS` stays set because nobody serviced it, no second message comes.
- **Failure scenario:**
  1. AppleAHCIPort's event source calls `disable()` around reset, sleep or error recovery, or the window between `registerInterrupt` and `enable()` is open.
  2. A command completes in that window.
  3. The VMD vector fires, the child is skipped, and the event is gone.
  4. After `enable()` nothing fires again, so the port hangs until the driver times out.
- **Apple's own behavior:**
  - `IOPCIMessagedInterruptController` doesn't drop. Its soft-disable doesn't stop dispatch: `handleInterrupt` checks only `interruptDisabledHard` (`IOPCIMessagedInterruptController.cpp:847-856`).
  - When a vector is hard-disabled it records `interruptRegistered = 3`, and `enableVector()` replays the handler from thread context (`:912-920`).
  - Legacy level-triggered interrupts stay asserted, so they fire again on enable.
- **Fix:** add a per-child `pending` flag:
  - set it in `vmdVectorFired` when the child is disabled but has a handler;
  - in `enableInterrupt`, clear it and call the handler once, as Apple's `enableVector` does;
  - or simply keep dispatching while soft-disabled, matching Apple's MSI semantics.

### 2. HIGH: `disableInterrupt` and `unregisterInterrupt` don't wait for a running handler. CONFIRMED (race exists; the window is small)
- **Where:** `VMDInterrupts.cpp:176-183`. The handler pointer is copied under `fLock`, the lock is dropped, then `h(t, r, n, 0)` runs. `:130-138` and `:155-160` don't synchronise with that.
- **Failure scenario:**
  1. CPU A is inside `h()`.
  2. CPU B's thread calls `IOInterruptEventSource::free()`, which calls `unregisterInterrupt`. It returns at once, and the event source and driver state are freed.
  3. CPU A is still running the filter on freed memory: a use-after-free or panic. This can happen on driver unload, termination or a failed start.
- **Same problem for disable:** a driver that disables and then tears down or reprograms state assumes the handler is quiescent, as IOKit's contract and `IOPCIMessagedInterruptController::disableInterrupt` (`:216-232`) guarantee by spinning on `interruptActive`.
- **Fix:**
  - give each child an `active` flag, set under the lock before the unlock and cleared after `h()` returns;
  - in `disableInterrupt` and `unregisterInterrupt`, when `!getPlatform()->atInterruptLevel()`, spin until `active == 0` after clearing `enabled`;
  - use the same flag as a test-and-set so one child's handler never runs on two CPUs at once (see finding 6).

### 3. HIGH: the child's MSI is never restored after a power transition. PLAUSIBLE
- **Where:** `VMDInterrupts.cpp:93-105` program MSI once, at `publishNub` time. Nothing re-runs it.
- **Why IOPCIFamily can't restore it:** it can't see the MSI capability, because phase 1 hides it.
  - `restoreDeviceState` writes back only the 64-byte header, plus the PCIe, LTR and L1PM capabilities (`IOPCIBridge.cpp:2325-2345`; `kIOPCIConfigEPShadowRegs = 16`).
  - Its MSI restore (`IOPCIMessagedInterruptController::restoreDeviceState`, `:726-730`) returns early because `msiCapability == 0`.
- **Failure scenario:**
  1. On system sleep the AHCI nub goes to D3hot through the visible PM capability (`setPCIPowerState`), or S3/S0ix removes power.
  2. On D0 entry with No_Soft_Reset = 0, or on power loss, the MSI enable, address and data reset to 0.
  3. After wake AHCI has no interrupt and I/O hangs.
- **Other reset paths:** FLR or hot reset on re-match (`IOPCIDevice.cpp:1078-1095`, `hardwareResetNeeded`) also wipes MSI.
- **Fix:** override the virtual `IOPCIBridge::restoreDeviceState(IOPCIDevice*, IOOptionBits)` (`IOPCIBridge.h:306`) in `VMDHostBridge`:
  - call `super`, then re-run the MSI programming for that nub;
  - split it out of `attachChild` into a `programMSI(child)` helper;
  - optionally also catch PMCSR D3→D0 writes in `configWrite16/32`, and the reset paths.

### 4. MEDIUM: VMD MSI-X may already be resolved, or have only one entry. PLAUSIBLE (fails cleanly or mis-indexes)
- **Where:** `VMDInterrupts.cpp:34-48`.
- **Case 1: lazy resolution already ran on the VMD nub.**
  - **Trigger:** anything calls `getProperty(IOInterruptSpecifiers)` or `saveDeviceState` on the VMD nub before our start. Examples: the first idle or sleep transition (`IOPCIDevice.cpp:833-837`), or `getNubResources` when the property pre-exists (`IOPCIBridge.cpp:4385-4391`).
  - **What that does:** `resolveInterrupts` has already run with `numRequired = 0`, so one platform vector is shared across the whole MSI-X table through sub-vectors (`IOPCIMessagedInterruptController.cpp:532-553`).
  - **What `configureInterrupts(MSIX, 2, 2)` does then:** it returns *success* without reallocating, because `msiPhysVectorCount >= 2` (`IOPCIDevice.cpp:1907-1912`).
  - **Consequences:**
    - IOKit indices no longer map 1:1 to entries, and legacy indices may come first.
    - The loop registers only the first 4 messaged indices.
    - Apple's shared-vector demux reads the PBA (`:833-840`) when `msiEnable > 1`, and on an unmasked entry the PBA bit is normally already clear once the message is sent. Child interrupts may then never be dispatched.
  - If the earlier resolution was legacy-only or non-MSI-X, the call returns `kIOReturnUnsupported`, and level 2 fails cleanly.
- **Case 2: the hardware table has one entry.** If the VMD's MSI-X table size were 1, the allocation loop (`:416-425`) breaks out *after* a successful one-vector allocation. That returns success even though `numRequired = 2`. Entry 1 wouldn't exist, so children would be silent.
- **Fix:**
  - read the VMD's MSI-X Table Size yourself and require it to be at least 2;
  - after `configureInterrupts`, require exactly 2 messaged indices with the `kIOInterruptTypePCIMessagedX` bit;
  - otherwise log the `IOInterruptSpecifiers` count and refuse level 2.
- **Fresh-resolution case:** in the normal case the mapping *is* deterministic in 726 sources. `numRequested = 2` makes `msiPhysVectors = numVectors = 2`, so the direct-vector path is taken, properties are appended in order, and only entries 0–1 are programmed (`:555-562`, `:614-627`). Index 1 is MSI-X entry 1.

### 5. MEDIUM (phase-1 fix side effect): advertising only the ABAR can starve the function's other BARs. PLAUSIBLE
- **Where:** `VMDBridge.cpp:145`. Only the ABAR's own range is advertised, and `sizeABAR` checks only BAR5.
- **Failure scenario:**
  1. The Intel SATA function implements another memory BAR, for example a non-zero BAR0–BAR4 memory BAR on some PCH generations.
  2. The configurator has to fit it into a range that is exactly ABAR-sized. Allocation fails.
  3. Depending on the configurator's pass, that BAR is left unassigned, memory decode is turned off, or the other BAR is placed at the ABAR's address and the ABAR is dropped.
- **Impact:** safety holds, because everything stays inside the AHCI function's own range, so this is not the NVMe hazard. The functional result is no ABAR decode.
- **I/O BARs:** no I/O window is advertised, which only disables I/O decode. That's fine.
- **Fix:** in `findAllowed`, read BAR0–4 and the ROM BAR of the allowed function. Then either:
  - refuse to start if any memory BAR other than BAR5 is non-zero or implemented, or
  - virtualise those BARs as unimplemented: reads return 0, writes are ignored.

### 6. LOW-MEDIUM: one child's handler can run concurrently on two CPUs. PLAUSIBLE (low probability)
- **Where:** `VMDInterrupts.cpp:39-47` registers *both* VMD vectors to `vmdVectorFired`, and every vector dispatches to every child.
- **Failure scenario:** entry 0 (software-trigger or spurious) and entry 1 fire on different CPUs. The same AHCI filter then runs twice at once, which IOKit normally never does for a single source.
- **Probability:** low if xnu x86 routes all device MSIs to the master CPU. I'm not sure it does.
- **Spurious dispatch:** a spurious dispatch on entry 0 is otherwise harmless, because AHCI checks `IS`.
- **Fix:** with finding 4's check in place, register only the index for entry 1, or use the test-and-set from finding 2.

### 7. LOW: unordered publication of `fChild[fCount]` versus `fCount++`. PLAUSIBLE (theoretical)
- **Where:** `VMDInterrupts.cpp:91-106`. `attachChild` can run on a PM or configurator thread while VMD vectors are live: `startVectors` ran first, and `publishNub` may be deferred until the bridge powers on.
- **The gap:** `fCount` isn't volatile and there's no barrier.
- **Why it's harmless today:** x86 TSO and `handler == nullptr` make it harmless in practice.
- **Fix:** write the child under `fLock` with `IOSimpleLockLockDisableInterrupt`, or issue `OSMemoryBarrier()` before `fCount++`.

### 8. LOW: details of the MSI register programming. CONFIRMED (minor) / layout NOT-AN-ISSUE
- **Where:** `VMDInterrupts.cpp:93-105`.
- **Layout is correct:**
  - address at +4;
  - 64-bit: upper address at +8 and data at +0xC;
  - 32-bit: data at +8;
  - MME (bits 6:4) cleared, then enable. Writing address and data before enable is the right order.
  - This matches FreeBSD `vmd_map_msi` (`0xFEE00000 | i<<12`, data 0) and Linux's dest-ID encoding.
- **Gaps:**
  - **Per-vector mask:** if `ctrl & 0x100` (per-vector masking), the mask bit is not cleared. That's +0x0C for 32-bit, +0x10 for 64-bit. It resets to 0, so it's only a risk if firmware left it set. Clear it explicitly.
  - **Extended Message Data:** Extended Message Data Enable (bit 10) is preserved from the read value. Clear it.
  - **INTx disable:** the 16-bit read-modify-write on the command register runs on the configurator thread without the bridge's `fLock`. That's acceptable, because no concurrent writer exists at publish time, and status (+6) isn't touched.
- **Can IOPCIFamily clobber it later? NOT-AN-ISSUE:**
  - every hidden MSI byte is write-blocked by `writeAllowed`;
  - the header restore writes the command register back with the saved value, which was saved after INTx disable was set;
  - even if INTx disable were cleared, an MSI-enabled function must not use INTx.
- **Bus master:** MSI needs Bus Master Enable on the child. AppleAHCIPort sets it, so this is fine, but be aware of it.

### 9. LOW: base `IOInterruptController` members are left uninitialised. NOT-AN-ISSUE (with one caveat)
- **No inherited path touches them:**
  - `vectors`, `controllerLock` and the expansion data are never read, because every entry point IOService uses is overridden: `register`, `unregister`, `getInterruptType`, `enable`, `disable`, `cause`.
  - `getInterruptHandlerAddress` and `handleInterrupt` are used only by cascaded controllers that call them on themselves.
  - `IOPlatformExpert::registerInterruptController` only stores the object (xnu, from memory).
- **`free()`:** bypassing to `IOService::free()` (`:28`) is fine. To my knowledge `IOInterruptController` has no `free` override. Unsure.
- **Caveat:** `lookUpInterruptController` retains the object, and the platform dictionary keeps it for life. So the controller is effectively immortal once `startVectors` succeeds, and `OSSafeReleaseNULL(fIC)` on the fail path (`VMDBridge.cpp:460`) never frees it. That's benign, because its vectors are stopped. Don't add anything to `free()` that assumes it runs.
- **Registration order:** the controller is registered before the bridge exists, so children can't block in `lookUpInterruptController` (which sleeps `THREAD_UNINT` until the name appears). That's correct.
- **Double registration:** `registerInterrupt` silently overwrites an existing registration. Apple returns `kIOReturnNoResources`. Minor.

### 10. NOT-AN-ISSUE: does a child's `registerInterrupt` reach `VMDInterruptController`? (question 1) VERIFIED
- **`IOPCIDevice` doesn't intercept:** it does not override `registerInterrupt`, `enableInterrupt`, `disableInterrupt` or `getInterruptType` (grep of `IOPCIDevice.cpp`). xnu's `IOService::lookupInterrupt` and `resolveInterrupt` read `IOInterruptControllers` and `IOInterruptSpecifiers` through the virtual `getProperty` (from memory).
- **What `IOPCIDevice::getProperty` does (`:1834-1876`):**
  - our pre-set arrays exist, so `value` is non-null and they aren't replaced;
  - then, once, it calls `parent->resolveInterrupts(nub)`. The parent is `VMDHostBridge`.
- **What `resolveInterrupts` (`IOPCIBridge.cpp:6158-6184`) does:**
  - **Legacy step:** `resolveLegacyInterrupts` returns `kIOReturnUnsupported` at the pin check, because the pin reads 0 through `virtualRead32`. That happens *before* `gIOPlatformSetDeviceInterruptsKey`, which is the call that would have overwritten the properties.
  - **MSI step:** `resolveMSIInterrupts` finds a platform MSI controller through `callPlatformFunction`. But `nub->reserved->msiCapability == 0`, because the capability is hidden in `publishNubs` (`:3852-3865`). So `allocateDeviceInterrupts`, which would *append* through `addDeviceInterruptProperties`, is never called.
- **Result:** nothing overwrites and nothing appends, so source 0 is our controller.
- **Dependency:** this relies on the pin and MSI hiding staying perfect. If the pin ever reads non-zero, the ACPI legacy route would replace our properties.
- **`configureInterrupts` from AppleAHCIPort:** `Messaged` or `MessagedX` returns Unsupported or BadArgument, because no capability is found. Then `interruptVectorsResolved` is reset, which is harmless.
- **Interrupt type:** `getInterruptType` returns Edge, so `IOInterruptEventSource` uses the non-auto-disable handler. That's correct.

### 11. NOT-AN-ISSUE: `publishNub` timing (question 2). VERIFIED
- **Order of calls:**
  1. `IOPCIBridge::start` → `configure` (the range is added) → `configOp(AddHostBridge)` → `probeBus` → `publishNubs`.
  2. `publishNubs` → our `publishNub` (`IOPCIBridge.cpp:4128-4139`) → `IOPCIBridge::publishNub`, which runs `attach` and then `registerService` (`:3362-3376`).
- **Result:** the properties are set before matching.
- **Config shadow:** the shadow's first `savedConfig` snapshot (`:3249-3254`) is taken *after* `attachChild`, so it includes INTx disable.
- **Early resolution:** `getNubResources` (`:4385`) will trigger the lazy resolution early, because our property now pre-exists. That's harmless, per finding 10.
- **Deferred probe:** if the bridge isn't powered (phase-1 finding 5), `publishNub` runs later from a PM or configurator thread. It is still before matching. See finding 7.

### 12. Phase-1 fixes re-check (question 6)
- **`goto fail` across initialisations: NOT-AN-ISSUE.**
  - `:391` jumps past the whole `{}` block (`:393-455`);
  - every later `goto` leaves that block;
  - no jump enters the scope of an initialised variable.
- **ABAR sizing: NOT-AN-ISSUE.**
  - decode is turned off before writing all-ones, and both are restored;
  - the read-back is verified;
  - `len = ~mask + 1` is correct because the low 4 bits are masked;
  - it rejects I/O and 64-bit BARs;
  - it runs before any driver binds, and DMA isn't affected.
  - Only BAR5 is considered: see finding 5.
- **Single-range `addBridgeMemoryRange`: correct for safety, with a functional caveat (finding 5).**
  - `AppleVTD::addMemoryRange` is also told about the range (`IOPCIBridge.cpp:4186-4188`), which is fine.
  - The `0x80000000/0x7f000000` special case can't match an ABAR range.
- **CFGBAR length check: NOT-AN-ISSUE.**
- **PM ordering: NOT-AN-ISSUE.**
  - The chain is VMD → controller → nub → bridge.
  - On the fail path, `fNub->PMstop()` may run on a nub that never got `PMinit`, if `init` or `attach` failed. To my knowledge xnu's `PMstop` returns early when not initialised (unsure).
  - `detach` of a provider that was never attached is a no-op.
- **Fail path after `fBridge->start` fails: LOW, PLAUSIBLE.**
  - `IOPCIBridge::start` may already have published the AHCI nub, and run `attachChild`, before failing.
  - The fail path then stops the VMD vectors and releases the bridge, but the configurator keeps a raw host-bridge pointer (phase-1 finding 6).
  - **Fix:** keep the bridge and controller alive once `start` has been called, even on failure, rather than releasing them.
- **ARL004 dummy read and phase-1 finding 4: NOT-AN-ISSUE.** The read in `vmdVectorFired` goes straight to ECAM, not through the bridge's `fLock`, so there's no interrupt-context deadlock.
  - **Caveat:** the "measured" rationale appears only in the local `linux-vmd.c` comment, not upstream as far as I know. The ordering argument still holds: a read completion can't pass the child's earlier posted writes.

## Recommended order of work
1. Add the active and pending flags: finding 1 (replay) and finding 2 (quiesce). Use the same flag to serialise (finding 6).
2. Add the `restoreDeviceState` override that re-programs MSI (finding 3).
3. Validate the VMD's MSI-X table size and index count after `configureInterrupts` (finding 4).
4. Check the AHCI function's other BARs (finding 5).
5. Minor items: clear the MSI mask bit and the Extended Message Data Enable bit, and add the barrier on `fCount` (findings 7 and 8).
