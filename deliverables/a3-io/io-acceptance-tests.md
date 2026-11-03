# I/O acceptance tests

Run these on the macOS target. Record each result as PASS, FAIL or N/A, with the macOS build, kext versions and date. Record logs with identifiers redacted: no MAC addresses or serial numbers.

**Common setup.** `log show --last 10m --predicate 'sender CONTAINS[c] "Lucy" OR sender CONTAINS[c] "AppleALC" OR sender CONTAINS[c] "IntelBluetooth" OR sender CONTAINS[c] "itlwm"'`, and `kextstat | grep -v com.apple` (or `kmutil showloaded`).

## Ethernet: Killer E3100G with LucyRTL8125

| ID | Test | Pass criterion |
|---|---|---|
| E-1 | Driver binds | `kmutil showloaded` lists LucyRTL8125. System Settings > Network shows the Ethernet service. `ifconfig en0` is active. |
| E-2 | Link speed | The link negotiates 2500baseT (or 1000baseT on a 1G switch) full-duplex, per `ifconfig en0 media`. |
| E-3 | DHCP / DNS | An IPv4 lease and an IPv6 SLAAC address arrive within 10 s of link-up. `ping -c 20` to the gateway shows 0% loss. |
| E-4 | Throughput | `iperf3 -c <LAN server> -t 30` and the reverse test `-R` both reach ≥ 2.2 Gbit/s on 2.5G, or ≥ 930 Mbit/s on 1G. Retransmits stay below 0.1% of segments. |
| E-5 | Sustained run | A 10-minute bidirectional iperf3 run (`--bidir`) has no link drops and no kernel panic. |
| E-6 | Cable reconnect | Unplug for 10 s and plug back in. The link and IP come back within 10 s, five times out of five. |
| E-7 | Sleep / wake | Run `pmset sleepnow`, then wake with the keyboard. Within 15 s of wake the link is up, the IP is still valid, and iperf3 passes E-4 again. Repeat 5 times. |
| E-8 | Promiscuous after wake | After wake, `tcpdump -i en0 -c 50` captures traffic. This is the v1.2.4 fix. |
| E-9 | Wake-on-LAN (optional) | Only if enabled in pmset: a magic packet wakes the system. |

## Wi-Fi: BE200 with the AirportItlwm-Tahoe alpha, phase 2 only

Skip this whole section (N/A) unless A4 has approved root patching.

| ID | Test | Pass criterion |
|---|---|---|
| W-0 | Binary match check | Before installing, `plutil -p AirportItlwm.kext/Contents/Info.plist` shows 0x272B8086 in IOPCIMatch, and the zip's SHA-256 equals 4bc3ab56…bcbf. If either check fails, stop. |
| W-1 | Driver binds | Loaded UUID matches the project's stated UUID (INSTALL:12). The Wi-Fi menu lists networks. |
| W-2 | Join WPA2 and WPA3 on 5 GHz | Associates within 15 s. DHCP succeeds. |
| W-3 | Throughput | `iperf3` reaches ≥ 300 Mbit/s down and ≥ 200 Mbit/s up at 5 GHz/80 MHz, 3 m from the AP. Record UDP loss separately; it is a known issue. |
| W-4 | Toggle | Wi-Fi off/on, 5 times. It rejoins every time. |
| W-5 | Sleep / wake | 5 cycles. Rejoins within 30 s of wake with no panic. |
| W-6 | Coexistence | During W-3, a BT audio device streams with no audible dropouts. |
| W-7 | Not-supported check | 6 GHz SSIDs are not expected. Record what happens; it is not a FAIL. |

## Bluetooth: Intel 8087:0036

| ID | Test | Pass criterion |
|---|---|---|
| B-1 | Controller up | System Information > Bluetooth shows a chipset and firmware version and is powered on. IntelBluetoothFirmware logs a firmware load. |
| B-2 | Pair HID | A BT mouse or keyboard pairs and works. |
| B-3 | Pair audio | A BT headset pairs. A2DP playback runs for 5 minutes without dropouts. The HFP microphone works in a test call. |
| B-4 | Reboot persistence | After a warm reboot and after a cold boot, paired devices reconnect automatically. |
| B-5 | Sleep / wake | After 5 sleep cycles, BT is powered and devices reconnect within 10 s. |
| B-6 | Wake from BT HID (optional) | A BT keyboard key wakes the system. Record the result; a FAIL here is acceptable. |
| B-7 | Continuity | Handoff and AirDrop are expected to fail. Record the result; it is not a blocker. |

## Analog audio: ALC1220 on 7F50

These tests apply only on Sequoia, or on Tahoe with AppleHDA restored.

| ID | Test | Pass criterion |
|---|---|---|
| A-1 | Device present | `ioreg -l \| grep -i "alc-layout-id\|layout-id"` shows the chosen ID. Sound settings list output devices. |
| A-2 | Rear line-out | A test tone plays clean stereo with left and right correct (`say -v` or Audio MIDI Setup tone). |
| A-3 | Front headphone | Plugging in switches output automatically or makes it selectable. No hum. |
| A-4 | Inputs | Rear line-in and the mic(s) show input level in Sound settings and record cleanly with QuickTime. |
| A-5 | Sleep / wake | Output still works after 5 sleep cycles with no need to replug. |
| A-6 | Digital (if fitted) | Optical/SPDIF passes 2.0 PCM. |

## HDMI/DP audio: RTX 5090 via NMHDA (A1)

| ID | Test | Pass criterion |
|---|---|---|
| H-1 | Device present | The monitor or TV appears as an output. |
| H-2 | Playback | Stereo plays at 48 kHz for 5 minutes with no dropouts. |
| H-3 | Hotplug | Disconnect and reconnect the cable. The output returns. |
| H-4 | Sleep / wake | The output returns after wake. |

## USB (after UTBMap)

| ID | Test | Pass criterion |
|---|---|---|
| U-1 | Map loaded | `ioreg -p IOUSB` or Hackintool shows only the mapped ports, at most 15 on the 7F6E controller. No `XhciPortLimit`. |
| U-2 | USB2 on every kept HS port | A USB2 flash drive enumerates on every mapped HS port. |
| U-3 | USB3 on every kept SS port | A USB3 drive enumerates at 5 Gbit/s or better (System Information shows "Up to 5 Gb/s" or 10). `dd` read is ≥ 300 MB/s on a fast drive. |
| U-4 | Type-C orientation | Both orientations work at SuperSpeed on each mapped C port, including the TCSS ports (UNRESOLVED-TCSS). |
| U-5 | Hotplug | 10 rapid plug/unplug cycles per port type with no hang and no "USB accessories disabled". |
| U-6 | Resume | Attached drives survive 5 sleep cycles. Keyboard and mouse wake the system. There is no instant wake (`pmset -g log \| grep -i wake` shows no XHC wake reason without user input). |
| U-7 | Internal | BT stays present (HS14 mapped as type 255). The AlienFX device is ignored if dropped. |
| U-8 | Shutdown / restart | No instant power-on after shutdown and no hang on restart. |

## USB audio fallback

| ID | Test | Pass criterion |
|---|---|---|
| F-1 | A class-compliant USB DAC or headset on a mapped **motherboard** port | It appears in Sound settings without a driver. A 10-minute playback has no glitches. |
| F-2 | Same after sleep | Works after wake. |
| F-3 | WD19TB dock audio 0BDA:402E (optional) | Record the result. A FAIL is expected while UNRESOLVED-TBT is open. |
