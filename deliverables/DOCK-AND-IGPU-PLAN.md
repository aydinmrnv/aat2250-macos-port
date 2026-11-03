# Dock (WD19TB) and Intel iGPU (7D67) under macOS: findings and build plan (2026-11-03)

The user requires the dock and the iGPU-driven monitors to work. This file covers what that takes, what exists, what has to be written, and the gates.

## 1. How these are wired today (DEMONSTRATED, Windows read-only queries 2026-11-03)
| Fact | Evidence |
|---|---|
| The AAT2250 has **no motherboard video outputs**. Its only iGPU-capable ports are the **2 rear Thunderbolt 4 Type-C** ports. | Dell AAT2250 spec page (rear ports list) |
| **Both LG Ultra HD monitors** are driven by the iGPU 8086:7D67 over **DisplayPort** (VideoOutputTechnology 10). The DELL S3225QS is on the RTX 5090. | `WmiMonitorConnectionParams` and monitor PnP parents (UID69 and UID4421) |
| So both LG signals leave through a TB4 port: either as DP tunnelled through the WD19TB, or as DP alt-mode on a direct USB-C cable. The UIDs suggest one of each, but this is **not confirmed**. Check the cables. | Inference (H) |
| Windows runs the **Microsoft USB4 software connection manager** (`USB4_MS_CM`) on host router 7EC2. The dock is a TB3 device (Titan Ridge 15EF) behind it. | PnP instance IDs |

## 2. What macOS needs for each piece (dependency chain)
1. **USB4 connection manager (CM):** builds the tunnels across the Thunderbolt link: PCIe to the dock's xHCI (USB, Ethernet, audio) and DP to the dock's monitor outputs. On this platform the CM is **software**. Windows supplies its own, Linux has `drivers/thunderbolt`, and **macOS has none for Intel integrated USB4**. Apple's IOThunderboltFamily drives Apple's own controllers only. No public hackintosh project solves this; a 2022 tonymacx86 thread asking about Tiger Lake's integrated TB4 has no answer.
   - Without a CM: no dock USB, no dock Ethernet, no dock audio, no dock monitors.
   - Possible shortcut, untested: BIOS Thunderbolt "pre-boot / boot support" may leave a PCIe tunnel up at handoff. That is a firmware setting change, so it is yours to make. It would not give hot-plug, and probably not DP tunnels.
2. **iGPU display engine driver:** the LG monitors need mode-setting on the Type-C DDI ports: Type-C PHY, PLLs, DP link training, two pipes. Hot-plug is also needed.
3. **Compositing beside the 5090:** WindowServer must drive monitors on two different GPUs, both on third-party drivers.

## 3. State of the art (untrusted third-party evidence, read 2026-11-03)
**XeGfx / WTLIntelGfx** (watchthelight-labs, v0.0.46, commit `ff64f836`, cloned for review to `~/macos-port-build/XeGfx` on the Mac Studio, **not built**):
- **What it does:** ports Linux i915 to XNU through FreeBSD drm-kmod/LinuxKPI, with Mesa ANV for Vulkan and a Metal plugin forked from nullmoth.
- **What it claims:** Metal, Vulkan and an accelerated desktop on **7D67**, our iGPU ID, under **Tahoe 26.7.2**. The testing was on two **laptops** (Core Ultra 9 275HX), for hours at a time. One panic (I-116) is still open.
- **Display limits:**
  - It only drives the built-in **eDP panel** on pipe A, adopting the firmware's mode. AUX A is hard-coded (`kexts/XeGfxIntelFB/intelfb.cpp:70-95`).
  - **External displays are not implemented:** `docs/xg_misc.md` says "returns -ENXIO until external displays (X6)".
  - There is no Type-C, Thunderbolt or multi-monitor support, and no desktop Arrow Lake-S test.
- **Licence:** mixed. Some parts are PolyForm Noncommercial, which is fine for personal use.
- **Relevance:** it is the best foundation by far. i915 already contains the full Type-C, DP and multi-pipe display code. The missing work is making that code path run on XNU and connecting it to IOFramebuffer.

There is no prior work at all for a macOS USB4 CM on Intel.

## 4. Build plan, with a gate at each step
Each phase needs a boot on your machine to pass its gate. I can write and review the code; only boots prove it.

| Phase | What gets built | Gate (acceptance) | Depends on |
|---|---|---|---|
| **D0** | Nothing new: observations added to the first test boot (dock connected, full `ioreg` dump, which monitors light up) | We learn whether firmware leaves a PCIe tunnel up and whether any LG monitor shows the boot screen | Test boot |
| **D1 USB4Probe** | A read-only kext, like VMDProbe. It reads host router 7EC2's NHI registers and the TBT root port 7EC4 link state. It sets up no DMA rings. | Kext loads, reports the NHI capabilities and the port/link state, and the system stays stable | D0 |
| **D2 TBCM-PCIe** | A minimal USB4 CM for **this one dock**: NHI control rings, router discovery, TB3-compat link to 15EF, PCIe tunnel. The USB4 spec (public) is the primary reference; Linux `drivers/thunderbolt` (GPL) is read-only reference, the same rule as VMD. | Dock xHCI 15F0 appears in macOS; dock USB keyboard, storage and audio 0BDA:402E work; it survives a cold boot ×10 | D1 |
| **D3 TBCM-DP** | DP tunnels from the iGPU DP-IN adapters to the dock's DP-OUT | A DP sink is detected through the dock (HPD and AUX reach the iGPU) | D2, G1 |
| **G1 iGPU modeset** | XeGfx's i915 path brought up for Type-C DDI ports with KMS: TC PHY, PLL, link training, 2 pipes, EDID, hot-plug. Done as a local fork of XeGfx, reviewed before every build. | One LG at 3840x2160@60 on a **direct** USB-C→DP cable, then both | VMD/install working; XeGfx reviewed |
| **G2 dual-GPU desktop** | The LG monitors are composited either by XeGfx on the iGPU or by NVIDIA with the iGPU only scanning out (nullmoth's NMIntelFB "head 4" design) | All 3 monitors live for 10 min, 5 hot-plugs, no WindowServer watchdog kill | G1 + the NVIDIA bring-up |
| **D4** | Hot-plug of the dock, and behaviour on shutdown and restart | Unplug and replug the dock ×5 with no panic | D3 |

**Honest size:**
- D2 and D3 amount to a new kernel driver of several thousand lines against an under-documented TB3-compat path.
- G1 makes the largest display subsystem in i915 run on XNU, inside an AI-heavy project that is two weeks old.
- Neither has ever worked on any hackintosh.

Expect months of iteration with many test boots. Failure at any gate is possible and will be reported as such.

## 5. Recommendation for getting macOS usable meanwhile
None of this blocks installing macOS. During bring-up:
- Cable both LG monitors to the RTX 5090. It has 3×DP + 1×HDMI, and you have 3 monitors.
- Plug essential USB devices into rear motherboard ports.

Windows keeps working the same way with that cabling. You can return the LGs to the iGPU whenever you like. The dock and iGPU phases then proceed in parallel and replace that setup once their gates pass.
