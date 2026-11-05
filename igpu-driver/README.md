# iGPU (8086:7D67) external monitors: the firmware-mode route

Goal: the two LG monitors, which Windows drives from the Arrow Lake iGPU through the rear Thunderbolt ports and the WD19TB,
working under macOS next to the RTX 5090.

Nobody has external monitors on a Meteor Lake / Arrow Lake iGPU under macOS (searched 2026-11-04). The closest work,
NMIntelFB and XeGfxIntelFB (watchthelight-labs), drives a laptop's built-in panel by adopting the mode the firmware
already lit and flipping frames into it. This directory tries the same idea on the Type-C DDIs, which only works if the
firmware leaves those pipes running when OpenCore hands over. The user reports the LGs wake (blank) at power-on.

## G0: IGPUProbe 0.0.1 (read-only)
- Matches 8086:7D67 in its own match category, so it never competes with NVRMFBClaim or IONDRV.
- Active only with `-igpuprobe`; `-igpuoff` disables it.
- Maps the first 2 MB of BAR0 uncached and read-only, reads per-pipe/transcoder state (TRANSCONF, TRANS_DDI_FUNC_CTL,
  timings, DP_TP, plane 1 CTL/SURF/STRIDE/SIZE), DDI_BUF_CTL for ports A-I, power wells, DPLLs, HPD and TCSS status, and
  the XELPDP Type-C port block, then unmaps. No writes, no BAR2, no forcewake.
- Results: `ioreg -lw0 -r -c IGPUProbe`.

Gate: the probe loads, the boot completes, and the dump shows which pipes/ports the firmware left lit.

Next (only if G0 shows lit Type-C pipes): G1, a framebuffer that publishes those pipes to WindowServer and flips
NVIDIA-composited frames into a GGTT-mapped surface (NVAccel head-4 copy path). If G0 shows nothing lit, this route is
closed and only the full modeset (i915 Type-C PHY, PLL and link training on XeGfx) plus a USB4 connection manager remain.

## G0 result, boot 1 (2026-11-04, LGs cabled as on Windows, NullMoth 2.7.1 active)
Probe loaded, boot clean. Raw dump: `IGPUProbe/boot1-ioreg.txt`.
- GMD_ID_DISPLAY 0x0380000C: display 14.00 (reads valid).
- **Pipe A is lit by the firmware:** TRANSCONF 0xC0000000 (enabled, active); TRANS_DDI_FUNC_CTL 0xA2010006 =
  DDI D (TC1), DP SST, 8 bpc, x4; DP_TP_CTL 0x80040300 (normal pattern, link trained); port clock 810000 kHz (HBR3).
  Timing 3840x2160, total 3920x2222 (CVT-RB, ~522.6 MHz, 60 Hz).
- **Plane 1 on:** 0x84000008, linear XRGB8888, 1920x1080, stride 7680 B, PLANE_SURF GGTT 0xFF800000, upscaled by
  pipe scaler 1 (PS1_CTRL 0x80000200). That is the "monitor on but blank" the user saw.
- **TC1 is in Thunderbolt mode:** XELPDP_PORT_BUF_CTL1(TC1) 0x31F09806 has IO_SELECT_TBT (bit 11), PHY ready (24),
  D2D link enabled (15). So the firmware set up a DP tunnel through the WD19TB.
- **Pipes B-D off, DDI buffers other than D off.** Only one LG is lit; the second is not driven at handoff.
- DPLL0-3 and TBT_PLL_ENABLE read 0 (TBT-mode clocking on this display version lives in PORT_CLOCK_CTL, 0x16F260 = 0x000CF402).

Consequence: the firmware-mode route can work for **one** LG (adopt pipe A, point plane 1 at a surface we own). The second LG
needs new link work (DP SST/MST over the existing TBT tunnel) and is a separate, harder step.

## G1: IGPUScanout 0.0.1 + lgmirror (built 2026-11-04, not boot-tested)
- `IGPUScanout/`: kext, inert without `-igpuscanout`. On a client's request it checks the firmware PTE for the plane at
  0xFF800000 is valid (proves the GGTT layout: PTEs at BAR0 + 8 MB), checks GGTT 0xF0000000-0xF4000000 is unused,
  allocates a write-combined buffer, maps it there with the firmware PTE's flag bits, and writes PLANE_SURF (mode 0) or
  also scaler/PIPESRC/size/stride (mode 1, native 4K). Everything is restored when the client detaches, closes or dies.
- `lgmirror/`: creates a CGVirtualDisplay of the plane's size, captures it with ScreenCaptureKit and copies frames into
  the mapped buffer. Needs Screen Recording permission.
- Gate G1a: `~/bin/lgmirror` (mode 0) shows the virtual display on the LG (1920x1080 upscaled), Ctrl-C returns it to black.
- Gate G1b: `~/bin/lgmirror --4k`. Risk: the firmware's DBUF/watermarks were set for a 1080 source; a 4K source may
  underrun (flicker/garbage). `lgmirror --info` prints PLANE_BUF_CFG and WM0 to judge that.

## G2: MST attempt (2026-11-04, IGPUScanout 0.0.3 + lgmst)
- Hub (upstream DPCD): DP 1.4 ext caps, HBR3 x4, MSTM_CAP 1, DSC supported, SINK_COUNT 2, branch OUI 90:cc:24 "SYNAS1" (Synaptics).
- Through the TBT tunnel the lanes report nothing during TPS1 and lock in TPS3; the DDI buffer must be cycled and TPS1 sent
  first, or the link drops when the pattern goes to normal. Retrain after the kext's undo, not before.
- MST upstream works: link trains with DP_TP_CTL MST, sideband works (LINK_ADDRESS: input port 0, SST sinks on ports 1 and 2,
  port 3 empty; ENUM_PATH_RESOURCES 2560 PBN each), payload table, ACT and ALLOCATE_PAYLOAD acknowledged, both transcoders
  active with no underrun.
- **Blocker:** the hub's downstream links to the LGs (DP 1.2, HBR2 x4) do not lock in MST mode: remote DPCD 0x202-0x204 read
  00 00 00/80 with symbol error counters saturated (0x7fff), on both ports, even the one without a stream. Only the very
  first attempt locked port 2 (white/gray picture). POWER_UP/DOWN_PHY, waking the sinks and a D3/D0 cycle of the hub did not help.
- Next idea: let Linux i915 (same display version) light both LGs in MST from a live USB with drm.debug and record the DPCD,
  sideband sequence and register state, then replay the difference.
