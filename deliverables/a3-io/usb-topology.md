# USB topology and port-map skeleton

Date: 2026-11-02. Author: agent A3.

## Method (read-only, no admin)

- `Get-PnpDevice` / `Get-PnpDeviceProperty` with the keys `DEVPKEY_Device_Parent`, `LocationInfo`, `LocationPaths` and `Address`.
- Standard hub query IOCTLs, the same ones USBView uses: `IOCTL_USB_GET_NODE_INFORMATION`, `..._NODE_CONNECTION_INFORMATION_EX`, `..._EX_V2` and `IOCTL_USB_GET_PORT_CONNECTOR_PROPERTIES`. Each was sent to each hub's interface handle.
- Script: `<scratch>/usbports.ps1`. It only sends queries; it changes no state.
- The "UserConnectable" and "TypeC" flags and the companion pairs come from firmware ACPI `_UPC`/`_PLD`, as Windows reports them. They are **DEMONSTRATED**, meaning this is what firmware declares. Firmware data can be wrong, so physical identity stays UNRESOLVED.
- No serial numbers or MAC addresses were collected.

## Correction to hardware-verified.md (DEMONSTRATED)

The following devices are **not on the motherboard**. They are inside a **Dell WD19TB Thunderbolt dock** that is attached through the CPU USB4/TBT root port (`PC00.TRP0`, root port 8086:7EC4) and then the USB4 host router 8086:7EC2:

- the 8086:15EF switch and the 8086:15F0 xHCI (Titan Ridge silicon inside the dock)
- the Realtek hubs 0BDA:0487, 5487, 0413 and 5413
- **0BDA:8153 USB GbE** and **0BDA:402E Realtek USB Audio**
- 413C:B06E/B06F (dock HID and firmware endpoints)

The USB4 router child is reported as "Thunderbolt 3(TM) Router, Dell - WD19TB Thunderbolt Dock".

**Consequence (HYPOTHESIS).** macOS has no connection manager for Intel's integrated USB4/TBT4 router. The dock will work under macOS only if firmware sets up the PCIe tunnel before boot (BIOS Thunderbolt pre-boot / boot support). Even then, hot-plug and sleep/resume of the dock are expected to be unreliable. Treat every device in the dock as **out of scope for first boot**.

## Controllers on the board

| Controller | PCI | ACPI | Root-hub ports | Devices attached now |
|---|---|---|---|---|
| PCH xHCI | 8086:7F6E subsys 1028:0CD6 | `_SB.PC02.XHCI.RHUB` | **26** (HS01-HS14, ports 15-16 with no protocol (likely USBR), SS01-SS10 = ports 17-26) | 19F5:2620 HID on HS04; 046D:C548 Logitech receiver on HS05; 05E3:0610 Genesys hub on HS11 (internal); 187C:0551 Alienware AlienFX on HS12 (internal); **8087:0036 Intel BT on HS14 (internal)** |
| CPU TCSS xHCI | 8086:7EC0 subsys 1028:0CD6 | `_SB.PC00.TXHC.RHUB` | 3 (port 1 USB2, not user-connectable; ports 2-3 USB3, Type-C) | none |
| USB4 host router | 8086:7EC2 | `_SB.PC00.TDM0` | n/a | WD19TB dock |
| Dock xHCI | 8086:15F0 behind 15EF | `PC00.TRP0.PXSX...` | 4 | Realtek hubs, then 8153 and 402E |

## Skeleton map: PCH 8086:7F6E (26 ports, limit 15)

Columns:

- **#** is the Windows port index, which is also the macOS `port` value. A USB3 port N is HSxx/SSyy as shown.
- **Proto** is the protocol from `EX_V2`.
- **Flags** are the ACPI-derived connector flags (UC = UserConnectable, C = Type-C).
- **Companion** is the companion port reported by Windows.
- **Guess type** is the macOS `UsbConnector` value (0 = USB2-A, 3 = USB3-A, 9 = Type-C with switch, 10 = Type-C without switch, 255 = internal).
- **Physical connector** is unresolved until the user plugs devices in.

| # | Name | Proto | Flags | Companion | Now attached | Guess type | Physical connector | Keep? |
|---|---|---|---|---|---|---|---|---|
| 1 | HS01 | USB2 | UC | none | empty | 0 | UNRESOLVED-PHYS | UNRESOLVED-KEEP |
| 2 | HS02 | USB2 | UC | none | empty | 0 | UNRESOLVED-PHYS | UNRESOLVED-KEEP |
| 3 | HS03 | USB2 | UC, C | SS07 (#23) | empty | 9 | UNRESOLVED-PHYS | yes (if C used) |
| 4 | HS04 | USB2 | UC | SS09 (#25) | 19F5:2620 HID | 3 | UNRESOLVED-PHYS | yes |
| 5 | HS05 | USB2 | UC | SS10 (#26) | 046D:C548 receiver | 3 | UNRESOLVED-PHYS | yes |
| 6 | HS06 | USB2 | UC, C | SS01 (#17) | empty | 9 | UNRESOLVED-PHYS | UNRESOLVED-KEEP |
| 7 | HS07 | USB2 | UC, C | SS02 (#18) | empty | 9 | UNRESOLVED-PHYS | UNRESOLVED-KEEP |
| 8 | HS08 | USB2 | UC, C | **7EC0 port 2** (cross-controller) | empty | 9 | UNRESOLVED-PHYS (USB2 lane of TCSS C port A) | UNRESOLVED-KEEP |
| 9 | HS09 | USB2 | UC, C | **7EC0 port 3** (cross-controller) | empty | 9 | UNRESOLVED-PHYS (USB2 lane of TCSS C port B) | UNRESOLVED-KEEP |
| 10 | HS10 | USB2 | UC, C | SS08 (#24) | empty | 9 | UNRESOLVED-PHYS | UNRESOLVED-KEEP |
| 11 | HS11 | USB2 | internal | none (hub port 4 lists #21) | 05E3:0610 hub, 4 downstream ports (all UC) | 255 | UNRESOLVED-PHYS (front panel or card reader?) | yes (internal hub) |
| 12 | HS12 | USB2 | internal | none | 187C:0551 AlienFX | 255 | internal | optional; no macOS driver, can drop |
| 13 | HS13 | USB2 | internal | none | empty | 255 | internal | drop |
| 14 | HS14 | USB2 | internal | none | **8087:0036 BT** | 255 | internal (M.2 BE200) | **yes, required for BT** |
| 15 | USR1 | none | none | none | status 3 | n/a | USB-R | drop |
| 16 | USR2 | none | none | none | status 3 | n/a | USB-R | drop |
| 17 | SS01 | USB3 | UC, C | HS06 | empty | 9 | UNRESOLVED-PHYS | UNRESOLVED-KEEP |
| 18 | SS02 | USB3 | UC, C | HS07 | empty | 9 | UNRESOLVED-PHYS | UNRESOLVED-KEEP |
| 19 | SS03 | USB3 | internal | none | empty | 255 | UNRESOLVED-PHYS (internal header?) | drop unless used |
| 20 | SS04 | USB3 | internal | none | empty | 255 | UNRESOLVED-PHYS | drop unless used |
| 21 | SS05 | USB3 | UC | port 4 (HS11's Genesys hub) | empty | 3 | UNRESOLVED-PHYS (front USB3 via hub?) | UNRESOLVED-KEEP |
| 22 | SS06 | USB3 | internal | none | empty | 255 | UNRESOLVED-PHYS | drop unless used |
| 23 | SS07 | USB3 | UC, C | HS03 | empty | 9 | UNRESOLVED-PHYS | yes (if C used) |
| 24 | SS08 | USB3 | UC, C | HS10 | empty | 9 | UNRESOLVED-PHYS | UNRESOLVED-KEEP |
| 25 | SS09 | USB3 | UC | HS04 | empty | 3 | UNRESOLVED-PHYS | yes |
| 26 | SS10 | USB3 | UC | HS05 | empty | 3 | UNRESOLVED-PHYS | yes |

**Port budget.** There are 12 user-connectable or needed HS ports (HS01-HS11 and HS14) plus 7 user-connectable SS ports (17, 18, 21, 23-26), for **19 total**, against a limit of 15. At least 4 must be dropped. A typical choice is to drop HS12, HS13, USR1-2 and SS03/04/06 (those are already excluded), and then also drop **4 of** the following:

- HS01/HS02, if they turn out to be unused USB2-only ports
- one Type-C pair, both its HS and SS halves
- the HS08/HS09 USB2 lanes of the TCSS C ports, if the user accepts that USB2-only devices will not work on those ports

Which ones to drop is decided as **UNRESOLVED-DECISION-1**, by the user, after physical mapping.

## Skeleton map: CPU TCSS 8086:7EC0 (3 ports)

| # | Name | Proto | Flags | Companion | Guess type | Physical | Keep? |
|---|---|---|---|---|---|---|---|
| 1 | HS01 | USB2 | internal | none | n/a | dummy lane | drop |
| 2 | SS01 | USB3 | UC, C | PCH HS08 | 9 | UNRESOLVED-PHYS (rear C, TBT4-capable?) | yes |
| 3 | SS02 | USB3 | UC, C | PCH HS09 | 9 | UNRESOLVED-PHYS | yes |

**HYPOTHESIS.** One of these two C ports carries the WD19TB dock today; that is TBT, not this xHCI. The TCSS xHCI is an ordinary xHCI class device (0x0C0330), so AppleUSBXHCIPCI should match it by class. Whether ports 2 and 3 enumerate in macOS is untested: Ice Lake and later TCSS behaviour on hackintoshes is inconsistent. Test this as **UNRESOLVED-TCSS**.

## Dock 8086:15F0 (out of scope)

The dock xHCI has 4 root ports: HS1, HS2 (Realtek 5487 hub), SS3 and SS4 (Realtek 0487 hub, then 8153 GbE and the 0413 hub). Do not map these ports in UTBMap. A dock map would only apply if the TBT tunnel exists at boot, which is UNRESOLVED-TBT.

## UNRESOLVED fields

- **UNRESOLVED-PHYS.** Which physical jack, front or rear, belongs to each port. The user must plug a USB2 device and a USB3 device into each jack. On Windows, USBToolBox needs only one USB3 device per jack because it binds companions.
- **UNRESOLVED-KEEP / UNRESOLVED-DECISION-1.** Which 15 ports to keep on the PCH controller.
- **UNRESOLVED-TCSS.** Whether macOS enumerates 7EC0.
- **UNRESOLVED-TBT.** Dock behaviour under macOS.
- Whether HS01 and HS02 are real rear USB2 jacks.
- Whether SS05 (#21) is the USB3 lane of a front-panel jack routed beside the Genesys hub.

## USBToolBox procedure on Windows (needs the user's approval)

This step requires the user to download and run a third-party executable. A3 did **not** do this. **The user must approve it explicitly.**

1. Download `Windows.exe` from USBToolBox/tool release 0.2 (https://github.com/USBToolBox/tool/releases/tag/0.2). The asset is 11,929,166 bytes (GitHub API) and has no published digest. Record its SHA-256 after downloading. Alternatively use `Windows.zip` (11,836,574 bytes), which is not a self-extractor. The source is pinned at 0c6823a2 (MIT).
2. Run it from its own folder. Drivers must be installed for all devices (README.md:59). Then choose **Discover Ports**.
3. Plug **one USB3 device** into every physical jack in turn: each front and rear Type-A, each Type-C in both orientations, and any front-panel jack. On Windows, companion binding fills in the HS half (README.md:41). Note the label for each port and record the jack in this file, replacing UNRESOLVED-PHYS.
4. Plug a USB2-only device into HS01 and HS02 candidates to confirm whether they are real jacks.
5. In **Select Ports**, keep at most 15 ports on 7F6E following DECISION-1, set the types (C ports to 9 or 10, internal BT to 255), and leave the 15F0 dock controller unmapped.
6. Press **K** to build the kext, choosing the USBToolBox-kext variant. That produces `UTBMap.kext`. Add `USBToolBox.kext` 1.2.0 and `UTBMap.kext`, and remove `UTBDefault.kext` (README.md:45-48).
7. Back up the generated `UTBMap.kext/Contents/Info.plist` to `deliverables\a3-io\` for review.
