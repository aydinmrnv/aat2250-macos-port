# ACPI tables for the AAT2250 (2026-11-02)

## SSDT-PLUG-ALT.aml
- **sha256:** `ba59b083120fb485f8552174e305a68d78be556dd0a5135e28c354092a4e449b`, 1601 bytes.
- **Source:** `SSDT-PLUG-ALT.dsl`, generated from the OpenCorePkg 1.0.8 sample (8d8d690c).
- **Built with:** iasl 20260408 on the Mac Studio. Result: 0 errors and 24 warnings. Every warning is the expected "Legacy Processor() keyword", because legacy `Processor()` objects are what macOS needs.
- **Values come from this machine's own tables:**
  - The DSDT declares Device `PR00`–`PR47` with `_HID` `ACPI0007` and `_UID` equal to the index.
  - The MADT has 24 enabled x2APIC entries with UIDs 0x00–0x17.
- **What the SSDT adds:** 24 Processor objects `CP00`–`CP17` (hex), with ProcessorID and `_UID` 0–23. They are visible only under `_OSI("Darwin")`. `plugin-type`=1 is set on CP00.
- **Check:** disassembled together with the DSDT, all 24 objects are present.
- **Status:** compiled; **untested on macOS**.

## Finding to test: the MADT has only x2APIC entries
All 24 CPUs appear as type-9 (Processor Local x2APIC) entries, even though every APIC ID fits in 8 bits (0x00–0x58). The table has no type-0 Local APIC entries.

**HYPOTHESIS:** macOS's CPU enumeration (in Apple's closed AppleACPIPlatform) may expect type-0 entries. The consequence could be only one CPU coming up, or a panic early in boot.

**Test on first boot:** compare `sysctl hw.logicalcpu` with 24, and keep the verbose boot log.

**Possible remedy if it fails:** a replacement MADT carrying type-0 entries with the same IDs, injected via OpenCore ACPI→Add with ACPI→Delete of the original. Not built yet. It would be machine-specific and needs the full table set.

## Still blocked
Superseded: the full dump is done (see below). Only SSDT-PMC remains unresolved.

## Full dump (2026-11-02)
- **Source:** all 24 SSDTs were exported without admin from `HKLM\HARDWARE\ACPI`, plus MCFG, HPET and LPIT via GetSystemFirmwareTable. MSDM was never read.
- **Location:** raw tables are in `evidence\acpi\`. Disassembly is on the Mac at `~/macos-port-build/acpi/full`.

**Findings**
- **PCI roots:** PC00 has `_UID` 0 (SoC), and PC02 has `_UID` 2 (PCH, bus 0x80).
- **Device paths:**
  - RTX 5090: `PciRoot(0x0)/Pci(0x6,0x0)/Pci(0x0,0x0)` (PC00.RP01.PXSX)
  - HDA: `PciRoot(0x2)/Pci(0x1F,0x3)`
  - PCH xHCI: `PciRoot(0x2)/Pci(0x14,0x0)`
  - TCSS xHCI: `PciRoot(0x0)/Pci(0xD,0x0)`
  - E3100G: `PciRoot(0x2)/Pci(0x1C,0x0)/Pci(0x0,0x0)`
  - BE200: `PciRoot(0x2)/Pci(0x1C,0x2)/Pci(0x0,0x0)`
- **EC:** the real EC is `\_SB.PC00.LPCB.ECDV` (PNP0C09). Its `_STA` always returns 0x0F, and Windows has it active.
- **Clocks:** AWAC (ACPI000E, gated by the GNVS field STAS) and the legacy RTC (PNP0B00, no `_STA`) are both active.
- **S3:** `_S3` exists but only `If (SS3)`. It is a firmware NVS flag, and S3 is currently unavailable.
- **PMC:** the PMC base comes from the GNVS field `PWRM`, which is dynamic. SSDT-PMC's address is therefore **UNRESOLVED**, and the table has not been built.

## SSDT-EC-USBX-AAT2250
- Compiles with 0 errors and 0 warnings. aml sha256 is `c2853263…edd81280`, built on the Mac.
- **Requires** an OpenCore ACPI→Patch: Base `\_SB.PC00.LPCB.ECDV._STA`, Find `5F535441`, Replace `58535441`, Count 1. Without it the table fails to load.
- The USBX values are the upstream defaults.

## SSDT-AWAC-DISABLE: built, collision check passed
- Compiles with 0 errors and 0 warnings. aml sha256 is `8437138164c829fe20900ab91ecd87bc8179aa57e711b4b410adc2d803f5662d`.
- **Collision check:** the earlier "conflict" was a grep false positive. The firmware defines `\_SB._INI` (DSDT, Scope `_SB`) and `\_SB.PC00._INI`, and no table defines a root `\_INI`, so this SSDT's `\_INI` loads cleanly.
- Still untested on macOS. On first boot, check that `ioreg` shows the RTC and no ACPI000E.

All `.aml` files are now copied back to this folder.

## APIC-LAPIC.aml (MADT replacement, 2026-11-03)
**The firmware MADT:** `evidence/acpi/APIC.aml`, 856 bytes, rev 5, OEM DELL. It has **no Processor Local APIC (type 0) entries**. Its contents:
- 48 x2APIC (type 9) entries: 24 enabled, with APIC IDs 0–88 and UIDs 0–23, plus 24 disabled 0xFFFFFFFF placeholders;
- 1 x2APIC NMI;
- 1 I/O APIC;
- 2 interrupt source overrides.

**Why it matters:** XNU counts CPUs only from type-0 entries. The fourth test boot hung right after `EB|LOG:EXITBS:START`, with no kernel output. Other Arrow Lake machines show the same symptom and were fixed the same way.

**The fix:** `x2apic_to_lapic.py` (our own script) converts every enabled x2APIC entry into a Local APIC entry with the same UID, APIC ID and flags, in firmware order, and drops the placeholders. It turns the x2APIC NMI into a Local APIC NMI (UID 0xFF) and copies all other structures unchanged. The result is 274 bytes with a valid checksum.

**How it's used:** OpenCore `ACPI → Delete` removes `APIC` with TableLength 856, and `ACPI → Add` loads `APIC-LAPIC.aml` first. The UIDs 0–23 match the `Processor` objects (ProcessorID 0–23) in `SSDT-PLUG-ALT`.

**Regenerating it:** after a BIOS update, re-dump APIC and rerun:

```bash
python3 -I x2apic_to_lapic.py APIC.aml APIC-LAPIC.aml
```
