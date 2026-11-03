# A2 platform analysis: Arrow Lake CPU, Dell ACPI, storage, Secure Boot

Machine: Alienware Area-51 AAT2250, board 02JGX1 A01, BIOS 1.20.0, Core Ultra 9 285K (family 6 model 198 = 0xC6 Arrow Lake-S), 64 GB DDR5.

Date: 2026-11-02. All diagnostics were read-only and run as a standard user. Nothing on the machine was changed.

Labels: **D** = DEMONSTRATED (observed on this machine or in pinned source), **P** = PROJECT CLAIM (third-party report), **H** = HYPOTHESIS.

Pinned sources (blob-filtered clones in `src/`, no scripts run):
- OpenCorePkg `8d8d690c` (tag 1.0.8 = `6fe4d15c`)
- nvidia-macos-driver `470e391e`
- 1401 `f5a92540`
- Ssuper2k/arrow-lake `8bc139e9`
- b00t0x/CpuTopologyRebuild `fe58fd88` (2.0.2)
- Lilu `0249c3a9` (1.7.2)
- VirtualSMC `b5c62c23` (1.3.8)
- RestrictEvents `9265e787` (1.1.6)
- CPUFriend `30c77338` (1.3.0)
- corpnewt/SSDTTime `d0fc80fb`

## 1. Arrow Lake under macOS — risk: HIGH

| Topic | Finding | Label |
|---|---|---|
| OpenCore support | 1.0.3 "Added Arrow Lake CPU detection" (Changelog.md:61). Model 0xC6 is in `ProcessorInfo.h:253`. `AppleCpuSupport.c:616` maps it with "FIXME - unknown, for now". Detection only; it adds no power management. | D |
| CPUID spoof | Required. macOS has no Arrow Lake CPU model. Both working configs spoof Comet Lake-S: `Cpuid1Data 55060A00 00000000 00000000 00000000`, mask `FFFFFFFF 0…`. 1401: without it the system hangs at EXITBS:START (`p1401/policy.py:270-274`). | P |
| ProvideCurrentCpuInfo | Not used by the 1401 225F config or the Asus 285K config. The CTR README says Arrow Lake does not seem to need it. | P |
| Hybrid topology 8P+16E, no HT | The MADT holds 24 enabled x2APIC entries plus 24 disabled ones. There are **no type-0 local APIC entries** (x2APIC only). APIC IDs: P-cores 0,8,32,40,48,56,80,88; E-cores in two 8-core blocks. CPU objects are `Device(ACPI0007)` `\_SB.PR00-PR23`, not `Processor()`. | D (MADT/DSDT read via Windows API) |
| CpuTopologyRebuild | 2.0.2 lists Arrow Lake as "experimental": 285K 24c24t → 8c24t. The sources conflict: 1401 disables it on desktop Arrow Lake (installer froze), while the Asus 285K runs it with `ctrsmt=full`. Treat it as a test variable; the draft has it off. | P |
| XCPM / power management | XCPM runs on the spoofed Comet Lake identity. Dell almost certainly locks MSR 0xE2 (no CFG-Lock menu), so the draft sets `AppleXcpmCfgLock=true`; verify with ControlMsrE2 (read-only). The CPU objects are Device-type, so the classic SSDT-PLUG (Processor-type) will not attach; use SSDT-PLUG-ALT-style Processor objects. The Asus combined SSDT does exactly this: CP00-CP23, plugin-type. P/E frequency curves are unknown; CPUFriend data for Arrow Lake does not exist upstream. Expect imperfect idle and boost behaviour. | D (object type) / P / H |
| TSC | Arrow Lake has invariant TSC and CPUID leaf 0x15. A single-socket desktop does not normally need CpuTscSync. Check by comparing `sysctl machdep.tsc.frequency` against the Windows-reported base clock and watching for clock drift. | H |
| macOS versions | **Sequoia 15.x:** Asus Z890/285K on 15.7.1 (Ssuper2k README, Reddit link inside); 1401's 225F reference machine on macOS 15 (`policy.py:270`) (P). **Tahoe 26:** 1401's `efi_rules.json:56` says the 225F "runs macOS 26", which contradicts `policy.py:270` (macOS 15). No independent 285K Tahoe report was found. A web search turned up only tooling pages such as [OpCore-Simplify](https://github.com/lzhoang2801/OpCore-Simplify). 1401 also caps RTX targets at macOS 15 (`docs/HOW-IT-WORKS.md`). Tahoe is Apple's last Intel release. **Recommendation: Sequoia 15.x first; Tahoe is unqualified.** | P |
| Sleep | Windows `powercfg /a`: "Standby (S3) — The system firmware does not support this standby state". Only S0 Low-Power-Idle and hibernate are available. FADT flag LOW_POWER_S0_IDLE=1. In the DSDT, `_S3_` is defined only inside `If (SS3)`. macOS needs S3, so **sleep/wake is likely unachievable** unless the Dell BIOS exposes S3, which needs a physical BIOS look. NullMoth's sleep work was tested on S3 desktops only. | D (powercfg, FADT) / H (macOS impact) |

## 2. Dell ACPI — what the tables show (read via Win32 `GetSystemFirmwareTable`, no admin)

**Tables present:** 43 total: DSDT (597,902 B, OEM DELL), 24×SSDT, DMAR, HPET, MCFG, FACP rev6, APIC, LPIT, NHLT, WSMT, PHAT, TPM2, BGRT, UEFI×2, MSDM, and others. The **MSDM table (Windows licence key) was deliberately not read.** Raw DSDT/FACP/APIC/DMAR copies were kept in the scratchpad only.

**Topology:** there are two PCI host bridges. `\_SB.PC00` is the SoC (PCIROOT(0)). `\_SB.PC02` is the PCH (PCIROOT(80), DSDT `_UID 2` by byte-scan, H). In OpenCore, PCH devices therefore probably live under `PciRoot(0x2)`, as in the Asus Z890 config.

| SSDT / fix | Needed? | Evidence |
|---|---|---|
| PLUG (XCPM) | **Yes, ALT variant** (Processor objects CP00-CP23) | PR00-PR23 are `Device` objects (D) |
| EC + USBX | **Yes.** Fake `EC` under `\_SB.PC00.LPCB` plus USBX. | Real EC = `\_SB.PC00.LPCB.ECDV` (PNP0C09) (D). No USBX in the DSDT (D). Whether to Darwin-disable ECDV is unresolved. |
| AWAC | **Recommended** (SSDT-AWAC-DISABLE, STAS=1 under Darwin) | `\_SB.PC02.AWAC` `_STA` returns 0x0F when STAS==0. `\_SB.PC00.LPCB.RTC` has no `_STA`, so it is always on (D). |
| PMC | Probably not | `PPMC` device already present (D). The OpenCore sample is 300-series only. Native NVRAM is untested (H). |
| SBUS/MCHC | Optional, cosmetic | `\_SB.PC02.SBUS` exists, MCHC missing, host bridge `MC__` (D) |
| RHUB / USB | A3 (USB map) | xHCI `\_SB.PC00.TXHC` (7EC0) and `\_SB.PC02.XHCI` (7F6E) (D). Per A3, 15EF/15F0 are inside a WD19TB dock. |
| DMAR | Drop in OpenCore | Present, flags 0x05 (D) |
| XOSI | No | Desktop, no I2C HID. `_OSI` up to "Windows 2022" (D) |
| HPET | No change planned | `\_SB.PC00.LPCB.HPET` present (D) |
| Dell PCHA/PCHS conditionals | Unknown | The Asus Z890 NOP patches' byte patterns occur in the Dell DSDT. Do not apply them without decompiling. |

**Unknown without a decompiled dump:**
- the exact `_STA` logic of ECDV
- `SS3` source
- the XHCI `_PRW`/GPRW wake methods (instant-wake)
- SSDT contents (Windows API returns only the first SSDT of 24)
- `_UID` confirmation
- `_DSM` on the HDA/GPU

### Read-only ACPI dump procedure (requires user approval; run by the user)

Option A, SSDTTime (recommended):
1. On the user's own decision, clone corpnewt/SSDTTime pinned at `d0fc80fb82e2488f3f481225221d2df356e94da6` into a new folder. Inspect `SSDTTime.bat` / `SSDTTime.py` first: it downloads ACPICA `iasl`/`acpidump` from intel/acpica or a mirror on first run. Approve that download explicitly.
2. Run `SSDTTime.bat` in a normal (non-admin) console. Select **"Dump the current system's ACPI tables"**. The output goes to `SSDTTime\Results\ACPITables\`.
3. Then use its **"Select ACPI table or folder"** option on that folder. Generate *only* FakeEC (laptop=No), PluginType (it will emit the CPU Processor-object variant), and AWAC. Do **not** let it write to any ESP.
4. **Delete or exclude `MSDM.aml` (and `SLIC.aml` if present)** before sharing anything. They contain the Windows product key. Never upload the dump publicly without reviewing it.

Option B, acpidump directly: `acpidump.exe -b`. Dumping tables does not normally need admin, but the tool is still a third-party binary, so it needs approval. Then run `iasl -da -dl *.aml`.

If either tool reports "access denied", stop. Do not elevate without a separate decision.

## 3. Storage — VMD covers EVERY internal disk (D)

PnP parent chains (Win32 `DEVPKEY_Device_Parent` / LocationPaths):

| Disk | Location path | Parent |
|---|---|---|
| SK hynix PCB01 **SED** 2 TB NVMe (Windows) | `\_SB.PC00.VMD0.SP21` | Intel RST VMD Controller **8086:AD0B** (`iaStorVD`) @ PCIROOT(0)#PCI(0E00) |
| WD60EFZX 6 TB SATA | `\_SB.PC00.VMD0.PRT4` | same VMD AD0B |
| Samsung 860 EVO 500 GB SATA | `\_SB.PC00.VMD0.PRT7` | same VMD AD0B |

- The two `8086:09AB "RST VMD Managed Controller"` functions are the remapped M.2 Gen5 root port (`PC00.RP21`, 00:01.0) and **the PCH SATA controller (`PC02.SAT0`, 80:17.0)**.
- **No standalone AHCI (class 0106) or NVMe (class 0108) controller is visible to the OS.**
- macOS has no VMD driver, so **no internal disk will be visible to the macOS installer**, and a SATA SSD in a bay would be under VMD too.

Dell's manual lists 3× M.2 2280 slots (slot 1 Gen5 boot; slots 2-3 Gen4) and up to two 2.5"/one 3.5" bays ([Dell storage spec](https://www.dell.com/support/manuals/en-uk/alienware-area-51-aat2250-gaming-desktop/alienware-area-51-r8-owners-manual/storage?guid=guid-4226b72a-07d7-4c50-9cb1-ba63adb3da8c&lang=en-us)). The empty slots' root ports do not enumerate. Whether Dell's "RAID On" also maps slots 2-3 under VMD is unknown (H: likely yes). Finding out needs physical inspection and a read-only look at BIOS → Storage.

| Option | Verdict |
|---|---|
| **External USB 3.2 Gen2 (10 Gb/s) NVMe/SATA enclosure** on a rear Intel xHCI port (PC02.XHCI 7F6E or PC00.TXHC 7EC0, not the WD19TB dock) | **Viable now, recommended.** No firmware or Windows change. OpenCore and macOS both live on that disk. Costs: USB mapping, a possible "disk not ejected" event on wake, and lower I/O than NVMe. |
| New NVMe in M.2 slot 2/3 | **Not viable unless that port is NOT VMD-mapped.** Unknown; needs BIOS inspection. If Dell maps it, it is invisible to macOS. |
| PCIe-slot NVMe adapter | Unknown. Slot availability with the RTX 5090 and VMD mapping of the slot's root port both need inspection. |
| SATA SSD in bay | **Not viable.** The SATA controller is behind VMD (D). |
| Switching BIOS storage mode RAID On → AHCI/NVMe | **Do not do this** without a separate, tested Windows migration plan. Windows boots through `iaStorVD`; switching gives INACCESSIBLE_BOOT_DEVICE ([Dell KB 000216532](https://www.dell.com/support/kbdoc/en-us/000216532/changing-the-storage-controller-mode-causes-windows-blue-screen-with-inaccessible-boot-device-error)). It would also expose the Windows disks to macOS. |

**SED / Opal note.** The Windows drive is a self-encrypting drive.
- Non-admin shell property `System.Volume.BitLockerProtection` = **2 for C:, D: and E:**. In the commonly documented mapping, 2 = "Off" (H; confirm with `manage-bde -status` as admin).
- Windows has defaulted to software encryption since 2019, but whether this drive has an Opal locking range or hardware encryption is unknown.
- Never run Opal/PSID-revert, sanitize, secure-erase or "format" tools against any existing disk. A PSID revert on an SED cryptographically erases everything.
- macOS cannot see this disk (it is behind VMD), which is itself a safety property for the experiment.

## 4. Secure Boot / BitLocker / VBS — options (no recommendation to act)

Current state:
- Secure Boot ON (registry UEFISecureBootEnabled=1).
- VBS running with HVCI.
- DMAR present, so VT-d is ON.
- BitLocker per-volume status reads 2 (likely Off, H).
- TPM query denied without admin.
- Firmware sleep is S0ix only.

**Core constraint.** OpenCore's `BOOTx64.efi`/`OpenCore.efi` are not Microsoft-signed, so with firmware Secure Boot on they will not start (D-class fact about UEFI; OpenCore Configuration.tex §UEFI Secure Boot says to sign with your own key). NullMoth also requires `SecureBootModel=Disabled`, and Configuration.tex:4683-4685 says images Apple Secure Boot does not handle are "handled by the firmware (as Microsoft Windows is)". So with firmware Secure Boot on, Apple's `boot.efi` would also have to pass firmware `db`, and it is not signed by any key in a PC's db (H, strong). **In practice the NVIDIA path requires firmware Secure Boot OFF while booting macOS.**

| Option | What changes | Consequences |
|---|---|---|
| A. Disable Secure Boot in BIOS (persistently) | A firmware setting | PCR7 changes. If BitLocker is on with a PCR7-bound TPM protector, the next Windows boot asks for the **recovery key** (Microsoft BitLocker FAQ). Windows still runs, VBS/HVCI still run, Windows 11 shows "Secure Boot off". Some anti-cheat titles refuse to start. Rollback = re-enable, which is another PCR7 change. |
| B. Toggle Secure Boot each time you switch OS | Repeated firmware setting changes | Every toggle is a PCR7 event, so recovery prompts can repeat. It is error-prone and has the worst usability. |
| C. Enrol your own keys (custom PK/KEK/db, keep Microsoft Windows Production CA 2011/2023 + UEFI CA), sign OpenCore and its drivers | Firmware key database | Allows OpenCore with Secure Boot on. **It still does not cover `boot.efi` with `SecureBootModel=Disabled`**, so it conflicts with the NullMoth requirement (H). It changes db/KEK, which is a PCR7 event. A mistake can make Windows unbootable until the factory keys are restored. Dell's "Expert Key Management" availability needs a look. |
| D. Shim (Microsoft 3rd-party UEFI CA) → OpenCore with MOK, `ShimRetainProtocol` | Shim on the macOS disk; possibly enabling the "Microsoft UEFI CA" option | Leaves the firmware db alone if the 3rd-party CA is already trusted. If not, enabling it is a db change. Same `boot.efi` problem as C. SBAT/dbx revocations can break it later. |

Other points:
- **Suspending BitLocker** (`manage-bde -protectors -disable C: -RebootCount 1`, admin) before any firmware Secure Boot change avoids the recovery prompt. That is the user's action, and only after they have confirmed they hold the recovery key.
- **VT-d:** keep it on. Dropping DMAR in OpenCore handles macOS without touching Windows' Kernel DMA Protection or VBS.
- **Fast Startup** is already disabled by policy (D). Keep it that way.
