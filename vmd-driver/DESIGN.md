# macOS driver for Intel VMD 8086:AD0B (Arrow Lake), Alienware AAT2250

Goal: let macOS Tahoe (x86_64) reach the SATA SSD (Samsung 860 EVO, "D:") that sits behind Intel VMD, without changing the BIOS storage mode, and **without exposing the Windows NVMe** that shares the same VMD domain.

Status: design + phase-0 code. Nothing has been loaded or run. Any test needs macOS already booted on the Alienware from another disk.

## Facts this design rests on

**This machine (read-only, Windows, 2026-11-02)**
- **VMD controller:** 8086:AD0B at PCIROOT(0)/PCI(0E,0), ACPI `\_SB.PC00.VMD0`. It has 32 MSI-X vectors (Windows lists 32 IRQ resources).
- **VMD BARs:**

  | Address | Size | Role |
  |---|---|---|
  | `0xB820000000` | 256 MB, 64-bit | CFGBAR, which is ECAM for 256 buses |
  | `0x72000000` | 16 MB | MEMBAR1 |
  | `0xB839000000` | 16 MB, 64-bit | MEMBAR2, which holds the MSI-X table |

- **Remapped functions:** two placeholders show as "RST VMD Managed Controller 8086:09AB" (class 0x088000):
  - PCIROOT(80)/PCI(17,0) = `PC02.SAT0`, the PCH SATA controller
  - PCIROOT(0)/PCI(01,0) = `PC00.RP21`, the CPU PCIe root port of the M.2 slot

  The real devices sit inside the VMD domain.
- **Disks whose Windows parent is the VMD:**
  - SK hynix SED NVMe (Windows/C:), at `VMD0.SP21`
  - WD60EFZX (E:), at `VMD0.PRT4`
  - Samsung 860 EVO (D:), at `VMD0.PRT7`

**Upstream sources (pinned)**
- **Linux `drivers/pci/controller/vmd.c` @ 2c88c2dc (v7.4-rc1).** GPL-2.0, so it is read as a reference only and no code is copied.
  - **Features for 0xAD0B:** `VMD_FEATS_CLIENT | VMD_FEAT_INTERRUPT_QUIRK`, where `VMD_FEATS_CLIENT` = MEMBAR_SHADOW_VSCAP | BUS_RESTRICTIONS | OFFSET_FIRST_VECTOR | BIOS_PM_QUIRK.
  - **No `CAN_BYPASS_MSI_REMAP`:** child MSIs must be remapped through the VMD's own MSI-X vectors.
  - **Bus window:** `VMCAP` (cfg 0x40) bit 0 means bus restriction is supported. `VMCONFIG` (cfg 0x44) bits[9:8] select the start bus: 0→0, 1→128, 2/3→224. The ECAM bus index is `bus - busn_start`, and the bus count is `CFGBAR size >> 20`.
  - **Erratum ARL004 (0xAD0B):** the VMD can signal its MSI before the child's posted DMA writes land. The documented workaround is a dummy read *to the child device* before handling the interrupt. A read that ends at the VMD is not enough.
- **FreeBSD `sys/dev/vmd/vmd.c` @ 57563954.** BSD-2-Clause, the primary design reference.
  - Lists 0xAD0B with `BUS_RESTRICT | VECTOR_OFFSET` and no bypass, which agrees with Linux.
  - Child MSI message: address `0xFEE00000 | (vmd_vector << 12)`, data `0`. Vector 0 is skipped (VECTOR_OFFSET).
- **Apple IOPCIFamily-726.100.6 (open source).** Its exact ABI must be matched to the IOPCIFamily inside macOS 26.7.2 before phase 1.
  - `IOPCIConfigurator::addHostBridge` takes an `IOPCIHostBridge` (exported from `IOPCIPrivate.h`, `__exported_push`) and **panics if the host bridge's provider is an `IOPCIDevice`**.
  - All config access goes through `hostBridge->configRead/Write{8,16,32}(space, offset)`.
  - Host bridge memory windows come from `addBridgeMemoryRange`. The bus range comes from `firstBusNum` / `lastBusNum`.
  - Child MSI goes through `callPlatformFunction(gIOPlatformGetMessagedInterruptControllerKey)`. Apple's x86 controller writes data=vector, which VMD remapping would misroute, so it must not be used for children.

## Architecture (phase 1+)

```
IOPCIDevice 8086:AD0B  (Apple-enumerated, PC00)
 └─ VMDController : IOService            matches AD0B; maps CFGBAR/MEMBAR1/MEMBAR2; owns VMD MSI-X vectors 1..31
     ├─ VMDInterruptController : IOInterruptController   demuxes VMD vector → child handler (with ARL004 dummy read)
     └─ VMDDomainNub : IOService (NOT an IOPCIDevice)
         └─ VMDHostBridge : IOPCIHostBridge
               configRead/Write → CFGBAR ECAM, with an allowlist filter
               firstBusNum/lastBusNum = VMD bus window; addBridgeMemoryRange(MEMBAR1)
               └─ IOPCIDevice (AHCI 0x0106xx only)  → AppleAHCIPort → Samsung 860 + WD60
```

### Safety policy (built into the config accessor, not optional)
- **Allowlist by function.** Only functions whose class code is SATA AHCI (`0x010601`) are visible to IOPCIFamily, plus the bridges strictly needed to reach them. For every other bus/device/function, `configRead*` returns all-ones ("no device") and `configWrite*` is dropped.
- **The NVMe stays hidden.** The NVMe root port and the SK hynix SED NVMe behind it (Windows and BitLocker/SED) are therefore never enumerated. macOS cannot mount, "initialize" or reconfigure them.
- **Limitation:** the WD60 (E:) shares the AHCI controller with the Samsung, so macOS **will** see it. macOS mounts NTFS read-only by default. Never accept "Initialize" for it in Disk Utility. It cannot be hidden per-port from config space, because AHCI port enable lives in MMIO.
- **Kill switch.** The boot-arg `-vmdoff` makes `VMDController::probe` fail. Phase 0 needs `-vmdprobe` to do anything at all.

### Interrupts
- **Children never use Apple's messaged-interrupt controller.** The host bridge hides the MSI/MSI-X capabilities from IOPCIFamily, and VMDController programs the child's MSI capability itself through CFGBAR: address `0xFEE00000 | (v << 12)`, data `0`, v ≥ 1.
- **Routing:** the child nub gets `IOInterruptControllers` / `IOInterruptSpecifiers` that point at `VMDInterruptController`.
- **Delivery:** VMD vector `v` is registered on the VMD's own IOPCIDevice through the normal Apple path. The VMD is an ordinary PCI device on PC00. On a VMD interrupt: do the ARL004 dummy read of the child's config/vendor register, then call the child's handler.

### Open risks (each needs a test on the real machine)
1. **Private ABI:** the `IOPCIHostBridge` vtable/ivar layout must match the macOS 26.7.2 IOPCIFamily, or the kernel panics. Mitigation: build against that exact tag, and check `OSMetaClass` sizes at start.
2. **Bus numbering:** the configurator may renumber bridges inside the VMD window. This is fine only if it stays within `busn_start .. busn_start+N-1`.
3. **BAR placement:** the configurator may reassign child BARs inside MEMBAR1. On bare metal MEMBAR offset = 0 (the shadow registers only matter for guests). Verify with phase 0.
4. **Power and sleep:** the VMD domain under sleep is out of scope. The platform has no S3 anyway.
5. **Delivery format:** whether AppleAHCIPort accepts interrupts delivered this way, given it may expect MSI semantics (`IOPCIMSIMode`).

## Phases
- **0. VMDProbe (this commit).** Read-only and safe to load.
  - Matches AD0B, maps CFGBAR read-only, and reads VMCAP/VMCONFIG and the VMD's MSI-X count.
  - Walks the ECAM bus window and publishes every function found (IDs, class, header type, BARs, capability IDs, bridge bus numbers) as the IORegistry property `VMDTopology`. It also logs them.
  - **Never writes** config space or MMIO.
  - Answers risks 2 and 3 and the real bus window.
- **1.** VMDHostBridge enumeration with the allowlist. No interrupts: AHCI attaches and then times out. This proves config and BAR mapping.
- **2.** The interrupt path, followed by read-only disk access tests (the Samsung only).
- **3.** Write tests on the Samsung only, after backups are verified, followed by the macOS install.

## Build
Built on the Mac Studio (Xcode 27, MacOSX27.0 SDK) with `build.sh`. There are no Xcode project files. The kext is ad-hoc signed and needs SIP kext-signing relaxed, which the NVIDIA driver already requires (csr 0x0A43).
