# config.draft.plist — notes and unresolved values (A2)

Status: **DRAFT - NOT BOOT READY** (also the top-level `#WARNING` key in the plist). Do not copy it to any ESP.

- Base: OpenCore **1.0.8 RELEASE** `Docs/Sample.plist`, sha256 `a58ca93e…0c96a58`. It is byte-identical to `git show 1.0.8:Docs/Sample.plist` in `src/OpenCorePkg`. The pinned HEAD `8d8d690c` is 1.0.8+20, so it is effectively 1.0.9-dev. Its Sample.plist differs: `background-color` NVRAM and an `apfs_aligned.efi` entry. I used the tag on purpose so the file matches the released ocvalidate.
- Generator: `scratchpad/tools/mkcfg.py`. It is deterministic and runs on the Sample.plist from the release zip.
- Validation: see `ocvalidate-output.md`. There is **1 error**, and it is expected: `SystemUUID` is a placeholder.

Labels: **D** = DEMONSTRATED on this machine or in pinned source, **P** = PROJECT CLAIM (third-party report), **H** = HYPOTHESIS.

## Key decisions

| Area | Value | Why / evidence |
|---|---|---|
| SMBIOS | `MacPro7,1` | Apple dropped iMacPro1,1 in macOS 26. The NullMoth troubleshooting doc says so (`docs/TROUBLESHOOTING.md:30` @470e391), and 1401 auto-switches iMacPro1,1→MacPro7,1 (`p1401/engine.py:859-864`, `efidoctor.py:61-64` @f5a9254). The driver README still says "tested: iMacPro1,1" (`README.md:127`), which is an internal inconsistency. The sleep/wake test was done on MacPro7,1 (`docs/SLEEP-WAKE.md:8`). MacPro7,1 has no iGPU, which matches "no iGPU in macOS". It covers both 15 and 26, and RestrictEvents hides its PCI/RAM slot nags (RestrictEvents README:14,27,38). iMacPro1,1 is only acceptable if you stay on Sequoia forever (P). |
| CPUID spoof | `Cpuid1Data 55060A00…`, `Cpuid1Mask FFFFFFFF…` (Comet Lake-S 0x0A0655) | The Asus Z890 285K config uses exactly this (Ssuper2k/arrow-lake `OC/config.plist` @8bc139e) (P). 1401 keeps the "upstream Comet Lake CPUID spoof": without it, "the kernel never starts: hang at EXITBS:START" (`p1401/policy.py:270-274`) (P). |
| ProvideCurrentCpuInfo | false | Not used by either working Arrow Lake config. The CTR README says Arrow Lake does not seem to need it (P). |
| CpuTopologyRebuild | present, **disabled** (test variable) | The sources conflict. 1401: the desktop Arrow Lake config does **not** load CTR, and with it the installer froze after "PCI configuration end" (`policy.py:270-289`, `efi_rules.json:42-49`) (P). The Asus 285K config loads CTR with `ctrsmt=full` (P). Start with it off. |
| DMAR | ACPI→Delete DMAR (All) + `DisableIoMapper=true` | 1401: every Core Ultra that booted had VT-d off; with DMAR present it hangs at EXITBS (`policy.py:291-307`) (P). This machine **has a DMAR table** (D, enumerated via GetSystemFirmwareTable). Dropping it in OpenCore only affects macOS. Do **not** turn VT-d off in the BIOS: Windows VBS/HVCI is running and needs it. |
| DevirtualiseMmio | true | 1401 rule `arrow-lake-desktop-devmmio` (`policy.py:324-327`). The Asus config has it on (P). The MmioWhitelist is empty; build it from the first OpenCore debug log. |
| SetupVirtualMap / RebuildAppleMemoryMap / SyncRuntimePermissions | true / true / true | Mirrors the Asus Z890 config (P). |
| ProtectUefiServices | false | Mirrors Asus. Test variable (H). |
| AppleXcpmCfgLock | true | Dell exposes no CFG-Lock toggle (H). Verify with the `ControlMsrE2.efi` read-only report (the tool entry is disabled). |
| ResizeAppleGpuBars / ResizeGpuBars | 0 / -1 | NullMoth installer-safe values (README "Wiring" table) (P). |
| Kernel→Block IONDRVSupport | present, **disabled** | NullMoth requires it post-install only. The installer must run without it (P). |
| SecureBootModel | `Disabled` | NullMoth requirement (`README.md:54,118`) (P). Consequence: see platform-analysis §4. UEFI Secure Boot cannot stay on in practice. |
| csr-active-config / boot-args | `430A0000` / `-v keepsyms=1 debug=0x100` + NullMoth args | NullMoth `README.md:51-53,99-100` (P). `amfi_get_out_of_my_way=0x1` disables AMFI system-wide. That is an A4 security decision and is left in only because the driver requires it. |
| ScanPolicy | `0x00200303` (FS lock + device lock + APFS + HFS + USB) | OpenCore will not list Windows or any internal (VMD/SATA/NVMe) volume. The Windows Boot Manager is never launched through OpenCore, so no macOS ACPI/SMBIOS changes reach Windows. Bits come from `Include/Acidanthera/Library/OcBootManagementLib.h:204-293`. If an internal non-VMD NVMe target is ever chosen, add `BIT19` (0x00280303). |
| LauncherOption / AllowSetDefault / RequestBootVarRouting | Disabled / false / true | OpenCore never writes Boot#### or BootOrder. Windows Boot Manager stays the firmware default. |
| ResetNvramEntry | disabled | An NVRAM reset can wipe firmware boot options on non-Apple firmware (Configuration.tex §ResetNvramEntry, Note 2). |
| Debug Target | 67 | Logs go to the OpenCore volume (1401 `boot-logs-on-stick` practice). |
| NVIDIA kexts | not injected | `README.md:92-93`: the kexts load from the AuxKC in /Library/Extensions. The OpenCore Kernel→Add entry is only a disabled marker. |
| A3 kexts | AppleALC, LucyRTL8125Ethernet, BlueToolFixup, IntelBluetoothFirmware, IntelBTPatcher, USBToolBox, UTBDefault enabled. UTBMap and AirportItlwm disabled. | Order and NVRAM BT keys were relayed from A3 by the coordinator; I have not re-verified them. |

## Every UNRESOLVED value

| Path | Value | Reason / who resolves |
|---|---|---|
| PlatformInfo→Generic→SystemSerialNumber, MLB, SystemUUID, ROM | `UNRESOLVED-GENERATE-LOCALLY` | The user generates these with GenSMBIOS/macserial, offline, on their own machine. Never commit or share them. ROM is a Data field and holds a placeholder string, so it needs a real 6-byte value. ocvalidate flags SystemUUID (expected). |
| NVRAM→Add→7C43…→prev-lang:kbd | `UNRESOLVED-user-locale-choice` | User preference. Should be Data, e.g. `en-US:0`. |
| DeviceProperties→PciRoot(0x2)/Pci(0x1F,0x3)→layout-id | `UNRESOLVED-A3-AppleALC-layout-candidates-…` | A3 has to test the Dell ALC1220 layout. This must be a 4-byte Data value. The `PciRoot(0x2)` path is H: it comes from a DSDT byte-scan (`_UID 0x02` near PC02), so confirm it with decompiled ACPI. |
| Kernel→Add UTBMap.kext comment | `UNRESOLVED-A3-USB-map-not-yet-generated` | Needs physical port testing under macOS. |
| Kernel→Add AMFIPass.kext comment | `UNRESOLVED-A1-A4-AMFIPass-source-and-hash` | Source, licence and hash need review. |
| Kernel→Add `UNRESOLVED-A1-NVIDIA-kexts-NOT-injected-by-OpenCore.kext` | marker, disabled | Documents that A1's kexts are not OpenCore-injected. |
| All kext/driver version pins (comments "version+SHA256 pin UNRESOLVED") | — | Nothing has been downloaded. Pin release + SHA-256 per kext before assembling an EFI. |
| HfsPlus.efi hash | — | From acidanthera/OcBinaryData; pin it. |
| ACPI→Add SSDT-PLUG-ALT.aml | file does not exist | The OpenCore sample (`Docs/AcpiSamples/Source/SSDT-PLUG-ALT.dsl` @1.0.8) declares 64 `CPxx`. Trim it to 24 (PR00-PR23 are `Device` objects in this DSDT, D) or generate it with SSDTTime from the dump. |
| ACPI→Add SSDT-EC-USBX.aml | file does not exist | The OpenCore sample targets `\_SB.PCI0.LPCB.EC0`. Here it must target `\_SB.PC00.LPCB`, and the real EC is `ECDV` (D). Whether to Darwin-disable ECDV on this desktop is undecided until the DSDT is decompiled. |
| ACPI→Add SSDT-AWAC-DISABLE.aml | file does not exist | The OpenCore sample can be used as-is (it sets `STAS=1` under Darwin). The DSDT shows the AWAC `_STA` is gated on STAS, and RTC has no `_STA` (D, byte-scan). |
| ACPI→Add SSDT-SBUS-MCHC.aml | disabled | Cosmetic. The sample assumes PCI0, but here SBUS is on PC02 and the host bridge is `MC__` on PC00. |
| Booter→MmioWhitelist | empty | Fill from the first OpenCore DEBUG log if DevirtualiseMmio breaks something. |
| Booter/UEFI quirk set | — | Copied from one Asus Z890 285K config plus 1401 rules. It is a Dell-specific test matrix item, not a known-good set. |

## Deliberately absent

- **No ACPI→Patch.** The Asus config's PCHA/PCHS "If → Noop" patches *do* match bytes in this Dell DSDT (4 and 10 raw hits). Applying them blind would remove conditionals Dell relies on. Only revisit after decompiling.
- **No SSDT-PMC.** OpenCore says the sample is for 300-series. This DSDT already has a `PPMC` device. Native NVRAM on Z890 is untested here (H).
- **No WhateverGreen.** No supported iGPU, and NullMoth needs `gfxrst=4` if WEG is present (`README.md:129`).
- **No XOSI.** Desktop, no I2C HID.
- **No RTC/AppleRTC kernel patches.**

## iGPU (8086:7D67): options
1. **Leave it unclaimed (draft default).** No macOS driver matches it. The monitor currently on the iGPU must be re-cabled to the RTX 5090 for macOS.
2. Hide it from macOS. This needs WhateverGreen `disable-gpu-support` (adds WEG and therefore `gfxrst=4`), or an SSDT `_STA` Darwin override. Only do this if a panic is traced to it.
3. BIOS iGPU disable: **not recommended.** It changes Windows' display topology and is a firmware change.

## Tahoe and ACPI pass (coordinator, 2026-11-02)
The previous version is kept as `config.draft.pre-tahoe.plist`. ocvalidate 1.0.8 (official .exe) on the new draft reports the same single expected error (placeholder SystemUUID). UNRESOLVED strings: 22.

**Target**
- New top-level `#TARGET` key: **macOS Tahoe 26.7.2 (25G332)**, offered by Apple on the Mac Studio and matching the BE200 alpha's test build. MacPro7,1 SMBIOS remains correct for Tahoe.

**ACPI**
The three SSDTs from `../acpi/` are referenced by name and SHA-256:
- `SSDT-PLUG-ALT.aml`
- `SSDT-EC-USBX-AAT2250.aml` (renamed from the generic SSDT-EC-USBX)
- `SSDT-AWAC-DISABLE.aml`

This **supersedes rows 47–49** of the UNRESOLVED table above:
- **SSDT-PLUG-ALT:** generated for exactly 24 cores from the MADT.
- **SSDT-EC-USBX:** the real EC ECDV is Darwin-disabled, per the upstream rule that AppleACPIEC must not load on desktops.
- **SSDT-AWAC-DISABLE:** collision check passed; the firmware has `\_SB._INI` but no root `\_INI`.

**ACPI→Patch:** one targeted entry, Base `\_SB.PC00.LPCB.ECDV._STA`, renaming `_STA` to `XSTA` in the DSDT (Count 1). It is required by SSDT-EC-USBX-AAT2250. This replaces "No ACPI→Patch" for this one case. The blind PCHA/PCHS patches stay excluded. Windows is unaffected because it boots from Windows Boot Manager, not through OpenCore.

**HDA path:** `PciRoot(0x2)/Pci(0x1F,0x3)` is now **DEMONSTRATED**. Windows reports PCIROOT(80)#PCI(1F03) = `_SB.PC02.HDAS`, and the decompiled DSDT gives PC02 `_UID` 0x02.

**Tahoe audio:** AppleHDA is absent, so AppleALC alone gives no analog audio. The options are:
- (a) restore AppleHDA via a root patch, which means more SIP reduction and redoing it after every update;
- (b) USB audio;
- (c) VoodooHDA.

This is a user decision. layout-id stays UNRESOLVED.

**Bluetooth on Tahoe:** the IntelBluetoothFirmware build variant is still UNRESOLVED (A3: the upstream release predates its Tahoe commit).

**SSDT-PMC:** not built. PWRM is a dynamic GNVS field, and Windows' resource map does not expose the PMC MMIO. Decide from evidence on first boot instead: write an NVRAM test variable, reboot, read it back. Build SSDT-PMC only if the variable does not persist.

**MADT is x2APIC-only:** first-boot check `sysctl hw.logicalcpu` = 24 (see `../acpi/README.md`).
