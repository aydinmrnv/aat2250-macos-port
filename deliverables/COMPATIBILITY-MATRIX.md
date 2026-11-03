# Compatibility matrix: Alienware Area-51 AAT2250 (2026-11-02)

The evidence level is the strongest one available. **DEMONSTRATED** means checked here (Windows-side, read-only) or proven by public reproducible evidence. **PROJECT CLAIM** means only the upstream project reports it. **HYPOTHESIS** means it has not been tested.
**No row has been tested under macOS on this machine.** Details are in `a1-nvidia/`, `a2-platform/`, `a3-io/` and `a4-security/`.

| Component | Candidate path (pinned) | Verdict for first boot | Evidence | Key risk / blocker |
|---|---|---|---|---|
| RTX 5090 10DE:2B85 (GB202), PCI match | nullmoth NVRM/NVRMFB/NVAccel @470e391 (v2.6.6) | Matches | DEMONSTRATED (source): the kexts match any NVIDIA display-class device; 2B85 appears only in the `install.sh` ID list | Recognition ≠ qualification |
| 5090 GSP/FSP boot | `gsp_ga10x.bin` (GB20X family), FSP wait shared GB202–GB207 | Plausible | Source: shared code; tested only on GB206/GB207 | No GB202 boot log exists publicly |
| 5090 display 4K@120, 3 monitors | NVKMS via NVRMFB | Unknown | PROJECT CLAIM for other cards; #454 open (3-monitor on 5080) | Must be tested; the iGPU monitor must move to the 5090 or stay unplugged |
| 5090 BAR1 32 GB (firmware-sized, `nvidia-smi`) | `NVRM::placeLargeBar1` | **Highest GPU risk** | Only 8 GB BAR1 proven; 16 GB resize failures reported (`NVRM.cpp:1043`) | Test 1 in `a1-nvidia/macos-gpu-tests.md` |
| 5090 Metal rendering correctness | Metal plugin → AIR→SPIR-V → NVK/NAK | Unknown | PROJECT CLAIM (5060 automated; 5070/5080 reports) | Pixel-diff tests needed; a device listing is not acceptance |
| 5090 compute | Same stack | Unknown | PROJECT CLAIM | CUDA not available (officially ended at 10.2) |
| 5090 for local AI models (user requirement 2026-11-03) | **No CUDA:** NVIDIA's CUDA user-space is closed source and ships for Linux/Windows only (last macOS release: 10.2, 2019), so it cannot be built here. Candidate instead: llama.cpp **Vulkan** backend on the driver's Mesa NVK, or its **Metal** backend | HYPOTHESIS; test after T-BOOT/T-DISP pass | llama.cpp Vulkan runs on NVK under Linux (public); untested on this macOS stack | Expect it to be well below CUDA speed if it works at all. Proven path today: CUDA in Windows/WSL2 on this same machine |
| 5090 media decode/encode | Closed `libnvdec`/`libnvenc` | Reported broken (#425) | PROJECT/ISSUE report without logs | 4 NVDEC/NVENC on GB202 vs 1 on GB206; the A1 patch adds instance diagnostics |
| 5090 sleep/wake | `nv_xnu_power.cpp` | Unlikely | Tested only on RTX 5050; VRAM copy is uncapped (32 GB) | Platform has no S3 (see below) |
| 5090 HDMI/DP audio 10DE:22E8 | NMHDA | Unknown | PROJECT CLAIM | — |
| Intel iGPU 8086:7D67 (drives both LG monitors over DP via the TB4 ports) | XeGfx 0.0.46 (i915 on XNU) + own Type-C modeset work | NO-GO for first boot; **build target** (see `DOCK-AND-IGPU-PLAN.md`) | PROJECT CLAIM: Metal on 7D67 laptops (eDP only); external displays not implemented upstream | Type-C DDI modeset is new work; cable the LGs to the 5090 until phase G1 passes |
| CPU Core Ultra 9 285K | CPUID spoof `55060A00` (Comet Lake), CpuTopologyRebuild disputed | HIGH risk | Third-party only (Asus Z890 on Sequoia 15.7.1) | Tahoe on Arrow Lake unproven; Sequoia first |
| ACPI (Dell DSDT) | SSDT-PLUG-ALT (CPUs are Device objects), EC/USBX, AWAC?, PMC? | Draft | DSDT partially read; 23 of 24 SSDTs not dumped | ACPI dump needs approval (exclude MSDM) |
| Storage | All internal disks behind VMD AD0B; no AHCI/NVMe exposed | **External USB SSD only** | DEMONSTRATED (PnP parent chains) | Never toggle VMD; M.2 slots 2–3 mapping needs a BIOS look |
| Sleep/wake (platform) | — | Expected to fail | DEMONSTRATED: `powercfg /a` → S0 Low Power Idle only, no S3 | Use shutdown instead of sleep until shown otherwise |
| Ethernet Killer E3100G 10EC:3000 | LucyRTL8125Ethernet v1.2.4 (96e981fc) | GO | Info.plist:51 matches 0x300010ec | Untested here |
| Wi-Fi BE200 8086:272B | kgp AirportItlwm-Tahoe alpha | NO-GO phase 1 | Upstream itlwm has no match; alpha needs root patch + AMFIPass | Phase 2 only |
| Bluetooth 8087:0036 | IntelBluetoothFirmware + IntelBTPatcher + BlueToolFixup | Conditional GO | Info.plist:87 match | No Continuity/AirDrop expected |
| Audio ALC1220 (subsys 1028:0CD6) on HDA 7F50 | AppleALC 1.9.8 | Sequoia: GO with layout search; Tahoe: NO-GO | 7F50 in Controllers.plist; no Dell layout | Layout-id candidates 1,7,11,5,3,2,16,28,29,30 |
| USB PCH 7F6E (26 ports) / TCSS 7EC0 | USBToolBox + UTBMap | Needs mapping | DEMONSTRATED topology from Windows | Physical plug test (USBToolBox run needs approval) |
| Dell WD19TB dock (USB4 router 7EC2 → TB3 15EF; USB audio 0BDA:402E, USB GbE 0BDA:8153) | Own USB4 connection manager kext (phases D1–D4) | NO-GO without a CM; **build target** | DEMONSTRATED: Windows uses its software CM (`USB4_MS_CM`); macOS has none for Intel USB4 | No prior art; see `DOCK-AND-IGPU-PLAN.md` |
| Firmware Secure Boot | — | Must in practice be off for the driver's `SecureBootModel=Disabled` | Per the OpenCore manual | User decision; BitLocker recovery risk |
| macOS security posture | SIP 0x0A43, AMFI off, `ipc_control_port_options=0` | Required by the driver | DEMONSTRATED (`README.md:51-53`) | Machine-wide reduction; see `a4-security/security-review.md` |
| Driver provenance | v2.6.6 release | Not traceable / not reproducible | No in-tree NVRM build script; no CI | See `a4-security/provenance-and-reproducibility.md` |

**Bottom line:** wired networking, Bluetooth and Sequoia analog audio have credible paths. Everything that makes this a 5090 machine (large BAR1, 4K@120 across monitors, correct Metal output, media engines, sleep) is untested on a GB202. The driver it depends on is unreproducible, and it requires turning off core macOS security.
