# I/O matrix: networking, Bluetooth, audio, USB

Date: 2026-11-02. Author: agent A3. Targets: macOS Sequoia 15.x or Tahoe 26.x (Intel), booted with OpenCore.

Labels:

- **DEMONSTRATED** means A3 read it at a pinned commit, or from Windows read-only queries on this machine.
- **PROJECT CLAIM** means the project's own docs say it, unverified.
- **HYPOTHESIS** means A3's inference.

Commits and hashes are listed in kext-inventory.md.

## Per-device summary

| Device (IDs) | Location (ACPI) | Candidate driver | Matching evidence | macOS constraints | Verdict |
|---|---|---|---|---|---|
| Killer E3100G 2.5GbE, 10EC:3000 subsys 1028:0CD6 rev 06 | `PC02.RP01.PXSX` | LucyRTL8125Ethernet v1.2.4 (96e981fc) | `LucyRTL8125Ethernet/Info.plist:50-51`: IOPCIMatch `0x812510ec 0x81251186 0x300010ec 0x812610ec 0x500010ec`. **0x300010ec is present (DEMONSTRATED).** | README says Catalina or later and "Supports AppleVTD (Tahoe included)" (PROJECT CLAIM). v1.2.4 fixes promiscuous mode after wake (commit message, PROJECT CLAIM). | **GO.** Primary NIC for first boot. |
| Intel BE200 / Killer BE1750x Wi-Fi 7, 8086:272B subsys 1A56:1774 rev 1A | `PC02.RP03.PXSX` | (a) upstream itlwm/AirportItlwm v2.3.0: **no match** (DEMONSTRATED; table ends at 0x2729, ItlIwx.cpp:11751-11766). (b) kgp-macPro AirportItlwm-Tahoe tag v1.2.0-be200-alpha.2 (e1eb70a8) | At the alpha tag, `itlwm/hal_iwx/ItlIwx.cpp:14928` has `IWL_PCI_DEVICE(0x272B, PCI_ANY_ID, iwl_gl_trans_cfg)` and :15317 has a dev-info entry (DEMONSTRATED). **However,** the IOPCIMatch at `AirportItlwm/Info.plist:32`, which is the plist the Tahoe targets use (project.pbxproj:3733-3884), **does not contain 0x272B8086**. Only `AirportItlwm-Monterey-Info.plist:32` does (DEMONSTRATED). The released zip's plist was not inspected: no download was made. | Alpha. Tested only on "macOS Tahoe 26.7.2 (25G332) … with AppleVTD enabled" (INSTALL_AND_FIRST_TEST.md:5). **Requires Retro-OCLP-CustoMac root patching + AMFIPass/`-amfipassbeta`** (same file, line 3). That conflicts with SIP and Secure Boot hardening, which is A4's concern. Not claimed: Wi-Fi 7, MLO, 6 GHz, AirDrop/AutoUnlock/full AWDL, Find My (STATUS.md; INSTALL:50-52). | **NO-GO for first boot. Experimental phase 2 (Tahoe only).** Blocked on checking the binary's matching and on A4 accepting root patches. On Sequoia, no BE200 path was found. |
| Intel BT, USB 8087:0036 (class E0/01/01) | PCH xHCI HS14, internal | IntelBluetoothFirmware + IntelBTPatcher + BlueToolFixup | `IntelBluetoothFirmware/Info.plist:76-89` has a personality with idProduct 54 (0x0036) and idVendor 32903 (0x8087). `IntelBluetoothFirmware.cpp:150-152` maps 0x0036 to `kTypeGen3` (TLV firmware naming at `IntelBluetoothOpsGen3.cpp:696-700`). The repo ships `fw/ibt-0291-0291.sfi/.ddc` (DEMONSTRATED). That 0291 is the right file for this module is a HYPOTHESIS: the name is computed at runtime from cnvi/cnvr_top. | The upstream release v2.4.0 (2024) predates the Tahoe support commit a414375 (2026-01). kgp's BE200 test used the **lshbluesky fork v2.5.1** with BlueToolFixup 2.7.2 for 8087:0036 (INSTALL_AND_FIRST_TEST.md:55-57, PROJECT CLAIM). BlueToolFixup needs two NVRAM variables (BrcmPatchRAM README:177-180) and has Tahoe bluetoothd patches in 2.7.2 (Changelog, PROJECT CLAIM). | **CONDITIONAL GO.** Basic HID and audio pairing is likely. Continuity, AirDrop, Handoff and Unlock with Apple Watch are **not expected** with Intel BT (HYPOTHESIS, consistent with the kgp scope list). |
| Realtek ALC1220 codec, HDAUDIO FUNC_01 VEN_10EC DEV_1220 **SUBSYS_10280CD6 REV_1001**, on Intel HDA 8086:7F50 | `PC02.HDAS` | AppleALC 1.9.8 (ff01db21) + AppleHDA | ALC1220 CodecID 4640 (0x1220) in `Resources/ALC1220/Info.plist:7-8`. Controller 0x7F50 has an AppleHDAController patch at `Resources/Controllers.plist:1473` ("800 Series(0x7F50) PCH HD Audio", MinKernel 17) (DEMONSTRATED). **No layout carries a Dell, Alienware or 1028:0CD6 entry** (grep of `Resources/ALC1220/`, DEMONSTRATED). | **Tahoe:** AppleALC README.md:21 says "macOS 26 dropped AppleHDA.kext in DP2" (PROJECT CLAIM, corroborated by the third-party project https://github.com/perez987/AppleHDA-back-on-macOS-26-Tahoe). Getting analog audio on Tahoe means re-adding AppleHDA from 26 beta 1 to the System volume. That needs SIP or root-volume changes (A4 decision) and must be redone after every update. **Sequoia 15.x: AppleHDA is present**, which is the normal path. | **Sequoia: GO** (layout trial needed). **Tahoe: NO-GO without root patching**; use USB audio, HDMI/DP or VoodooHDA instead. |
| RTX 5090 HDMI/DP audio, 10DE:22E8 (codec 10DE:00AA) | `PC00.RP01.PCI(0001)` | NMHDA.kext in nvidia-macos-driver @470e391e (`kexts/NMHDA/`) | owned by A1 | It depends entirely on A1's GPU driver working. | **Deferred to A1.** |
| Intel iGPU display audio, codec 8086:281D | HDAS SGPC function | none | n/a | The Arrow Lake iGPU has no macOS framebuffer (HYPOTHESIS; A1/A2 own this). | **NO-GO.** |
| Realtek USB Audio 0BDA:402E (MI_00 class 01/01/00 = UAC1) | **Inside the Dell WD19TB dock** (via 15F0 and Realtek hubs), not the motherboard (DEMONSTRATED; see usb-topology.md) | native AppleUSBAudio (class driver) | The UAC1 class code is DEMONSTRATED from Windows CompatibleIds. | It works only if the dock's TBT tunnel exists under macOS (UNRESOLVED-TBT). | **Fallback only, HYPOTHESIS.** A generic UAC device on a motherboard port is the reliable fallback, especially on Tahoe. |
| Realtek USB GbE 0BDA:8153 rev 3111 (class FF in the config Windows uses) | **Inside the WD19TB dock** | native AppleUserECM/NCM if the device exposes an ECM/NCM configuration (HYPOTHESIS) | Windows shows only the vendor-class config. | Same TBT dependency. | **Not a first-boot path.** |
| USB: PCH xHCI 8086:7F6E (26 ports), TCSS xHCI 8086:7EC0 (3 ports) | `PC02.XHCI`, `PC00.TXHC` | AppleUSBXHCIPCI (class match) + USBToolBox 1.2.0 + UTBMap | Port data is in usb-topology.md (DEMONSTRATED from Windows). | The PCH has 26 ports against a limit of 15, so mapping is mandatory. XhciPortLimit is broken on 11.3 and later (PROJECT CLAIM). | **GO after mapping.** Use UTBDefault until the map is built. TCSS is UNRESOLVED. |
| Dock xHCI 8086:15F0 / USB4 router 8086:7EC2 | `PC00.TRP0` / `PC00.TDM0` | none (no Intel USB4 connection manager in macOS) | n/a | n/a | **NO-GO / out of scope** (HYPOTHESIS). Boot without the dock attached, or expect no hot-plug. |

## Audio: codec IDs and layout-id plan

**What Windows reports (DEMONSTRATED):**

- `HDAUDIO\FUNC_01&VEN_10EC&DEV_1220&SUBSYS_10280CD6&REV_1001`: the codec is ALC1220 (not reported as S1220A), the subsystem is Dell 1028:0CD6, which matches the board's PCI subsystem, and the revision is 0x1001.
- GPU HDA: `VEN_10DE&DEV_00AA&SUBSYS_10DE0000&REV_1001`. iGPU: `VEN_8086&DEV_281D&SUBSYS_80860101`.

**What this means:**

- AppleALC has **no OEM layout for this board**, so the pin configuration is Dell-custom and unknown.
- The **ALCS1220A** layout folder uses CodecID 4456 (0x1168) in `Resources/ALCS1220A/Info.plist:7-8`. This codec reports 0x1220, so those layouts **should not apply** (HYPOTHESIS).
- Generic desktop layouts are the first things to try.
- The definitive approach is a codec verb dump from a Linux live USB (`/proc/asound/card*/codec#0`). That does not need admin on Windows, but it does need the user to boot Linux. Use the dump to pick a layout or to write a custom one.

**Candidate `layout-id`s, in order to try** (from `Resources/ALC1220/Info.plist` comments):

1. **1**: Toleda, 5/6 ports, 2 inputs, 3/4 outputs, front panel, SPDIF
2. **7**: Mirone generic
3. **11**: truesoldier custom
4. **5**: Mirone generic
5. **3**: Toleda, 3 ports, 2 inputs, 1 output, front panel
6. **2**: Toleda, 3 ports repurposed to 5.1
7. **16**: MacPeet Gigabyte Z390
8. **28** / **29**: Z390 Aorus Ultra, manual or switch mode
9. **30**: Z370 Aorus Gaming 7, switch mode
10. **27**: Z370-HD3P
11. **13**: Clevo P950HR
12. **20**: Gigabyte B550 Vision D

The laptop layouts (15, 21, 25, 34, 35, 98, 99, 100) are a low priority.

**How to test** (Sequoia, or Tahoe with AppleHDA restored):

1. Boot with `alcid=<n>`, which is faster than editing DeviceProperties. A2 should allow the boot-arg.
2. For each candidate, open System Settings > Sound and record which outputs and inputs are listed.
3. Play test tones from each rear jack and the front headphone jack. Record from the rear line-in, the rear mic and the front mic.
4. Check headphone plug/unplug switching and SPDIF/optical if the case has it.
5. Record the results in io-acceptance-tests.md (A-1 to A-6), then lock the chosen value in `DeviceProperties` as layout-id.
6. Debug with the AppleALC DEBUG build and `-alcdbg`, and check `ioreg -l | grep -i layout`.

## Networking order for first boot

1. **The onboard Killer E3100G with LucyRTL8125.** Use a cable to the router. This is the only network path that does not depend on an alpha driver or the dock.
2. **USB fallback:** a known macOS-native USB-C or USB-A Ethernet adapter (for example an AX88179 or an RTL8153 adapter that has an ECM configuration) on a **mapped motherboard port**, not on the dock.
3. **Wi-Fi BE200:** only in phase 2 on Tahoe, after A4 accepts root patching. Its current ceiling is Wi-Fi 6/6E-class behaviour on 2.4 and 5 GHz. **6 GHz, Wi-Fi 7 (320 MHz) and MLO are not claimed** (kgp INSTALL:50-52). Expect no AirDrop/AWDL. Find My Mac is unresolved (STATUS.md).
4. **WD19TB dock Ethernet:** do not rely on it (UNRESOLVED-TBT).

**Bluetooth and Continuity caveats.** Intel BT through BlueToolFixup gives a basic HCI. Handoff, Universal Clipboard, AirDrop, Sidecar, Apple Watch unlock and Continuity Camera need Apple or Broadcom BT plus AWDL Wi-Fi, and **are not expected** (HYPOTHESIS). The only way to get full Continuity would be a Broadcom/Apple Fenvi-class card (BCM94360/943602). That card needs AirportBrcmFixup or OCLP root patches on Sonoma and later, so it is a hardware swap, out of A3's scope, and listed as an option only.

## Sources

- Pinned clones in `src\`. File:line citations are in the table above.
- AppleHDA removal in Tahoe: AppleALC README.md:21 @ff01db21, and [perez987/AppleHDA-back-on-macOS-26-Tahoe](https://github.com/perez987/AppleHDA-back-on-macOS-26-Tahoe).
- GitHub Releases API metadata for release tags, sizes and digests.
