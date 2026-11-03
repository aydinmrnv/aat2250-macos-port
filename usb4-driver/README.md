# usb4-driver: USB4 connection-manager work for the WD19TB dock (plan: `../deliverables/DOCK-AND-IGPU-PLAN.md`)

## USB4Probe (phase D1): read-only
- **Matches:** NHI 8086:7EC2 (`IOPCIMatch 0x7ec28086`, score 0).
- **Gating:** it is inert unless the boot-arg `-usb4probe` is set; `-usb4off` disables it.
- **Build:** `USB4Probe/build.sh` on the Mac Studio (Xcode SDK, x86_64, ad-hoc signed).
  - Binary sha256: `ad72ef6b9b3cc6f8f9d1a9a9f306d2b68d05e39ca5712592d5399179853898bb`.
  - The build has no warnings except ld's usual note that libkmod.a was built for 27.0.

### What it publishes (IORegistry, class `USB4Probe`)
| Property | Meaning |
|---|---|
| `NHICommand`, `NHIPMCSR`, `NHIBAR0`, `NHIMSIXCount` | PCI state. MMIO is only read in D0 with memory decode on. |
| `NHIRegisters` | A fixed allowlist of 16 MMIO dwords: ring 0 TX/RX base, index, size and flags; CAPS; DMA_MISC; RESET; INMAIL/OUTMAIL; FW_STS |
| `NHIHopCount`, `NHIVersion` | From CAPS |
| `NHIFirmwareCM`, `NHINVMAuthDone` | **The key D1 answer:** is Intel's firmware connection manager (ICM) running? |
| `TBTRootPorts` | For each root port 7EC4–7EC7: bus window, PCIe link status (DLL active, width, speed), slot capabilities/status, and every PCI function macOS enumerated behind it |

### Review (2026-11-03, self-review against the D1 rules)
- **No writes anywhere.**
  - The only config accessors used are `configRead16/32`.
  - The BAR is mapped `kIOMapReadOnly`, only its first 256 KB, and it is released before `start` returns.
- **Clear-on-read registers are excluded.** REG_RING_NOTIFY_BASE (0x37800+) is cleared on read (Linux `nhi.c` comment), and neither it nor REG_RING_INT_CLEAR is in the allowlist. The other allowlisted registers are plain read/write registers that Linux also reads.
- **No power changes.**
  - If the NHI is not in D0, the probe reports that and does not read MMIO.
  - CAPS = 0xFFFFFFFF is treated as "not responding".
- **Registry iteration only reads.** It reads vendor and device IDs of each IOPCIDevice and config space of the root ports. Iterators are released.
- **Known limitation:** devices behind a root port that IOPCIFamily publishes *after* USB4Probe starts are missing from `enumerated-below`. The `ioreg-full.txt` capture from the test boot covers that.
- **Interpretation guide for the result:**
  - **ICM_EN = 1 and NVM_AUTH_DONE = 1:** firmware CM mode. D2 becomes a small ICM client (driver-ready, approve the device) instead of a full software CM.
  - **ICM_EN = 0:** a full software CM is needed (the D2 plan as written).
  - **DLL up on a root port with 15EF/15F0 below it:** firmware left a PCIe tunnel up.
