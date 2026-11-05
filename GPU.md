# RTX 5090 driver (NullMoth 2.7.1) on the AAT2250

The community NVIDIA driver [nullmoth/nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver) gives
macOS a real driver for RTX 20–50 cards: native resolution, refresh rate and Metal acceleration. It's not from NVIDIA.
Its licence is PolyForm Noncommercial 1.0.0, so personal use only. **The RTX 5090 (2B85) is on its supported-ID list,
but this card hasn't been reported working yet.** RTX 5060 is tested by the project, and 5070/5080 are reported working.

## What was checked (2026-11-05)
| Item | Pin | Check |
|---|---|---|
| `nullmoth-nvidia-2.7.1.tar.gz` | sha256 `0b28f420dbc0f8a796a8a67c60490b1909886d302fd2008ad884cc12231e7851` | **Ed25519 signature verified** with the NullMoth release key (`ztPmcj+m…d4E=`, the key compiled into its 1401 installer); matches the GitHub digest and `SHA256SUMS.txt`; all 77 inner files match `SHA256SUMS` |
| `install.sh` | identical to repo `6b4752c` | read in full at 2.6.6 (`deliverables/a4-security/security-review.md`). The 2.6.6 → 2.7.1 diff only adds NMXAudio (below) and a `chmod 755 /Library/NullMoth` |
| `AMFIPass.kext` 1.4.1 (dhinakg) | zip sha256 `07b266145906db41f4b13a7938fbb173ea28888cc1fa65f84417f8820adc961e` | from Dortania's OpenCore-Legacy-Patcher `payloads/` (commit `76516394`, 2024-06-28); same pin as NullMoth's 1401 installer. Closed source. |

**Two new kernel drivers go in:**
- **The NVIDIA kexts:** NVRM, NVRMFB, NVRMAGDC and NVAccel.
- **NMXAudio:** on macOS 26 with an HD Audio controller, `install.sh` adds this FreeBSD-derived audio driver. It could bring the onboard audio jacks to life. It is also new kernel code.

**What the driver needs in OpenCore** (`tools/make-postinstall-config.py`, written to `EFI/OC/config-nvidia.plist`):
- **boot-args:** `amfi_get_out_of_my_way=0x1 amfi=0x80 ipc_control_port_options=0 nvfb=1 nvaccel=1 nvfbheads=4 -nvkmsnosmooth`. This **turns off macOS's code-signing enforcement (AMFI)**, which is the trade-off this driver requires.
- **Kernel > Block:** `com.apple.iokit.IONDRVSupport`, Strategy Exclude.
- **Kernel > Add:** `AMFIPass.kext` (after Lilu).
- **Already set:** SIP 0x0A43, SecureBootModel Disabled.
- **Debug log:** errors only.

## Install
1. Boot macOS from the **Lexar** and finish setup.
2. Terminal:
   ```bash
   sudo sh /Volumes/OPENCORE/gpu
   ```
   It checks the package hash, unpacks it, checks every file, then runs the driver's own `install.sh`. Only if that succeeds does it switch the stick to `config-nvidia.plist`; the previous config is kept as `config-basic.plist`. The log goes to `gpu-install.txt` on the stick.
3. **System Settings → Privacy & Security:** if it asks to allow NullMoth software, click Allow.
4. **Restart.** The first start can pause up to a minute at "PCI configuration end".
5. Check:
   ```bash
   system_profiler SPDisplaysDataType | head -20
   ```
   It should show the GeForce RTX 5090 with "Metal: Supported".

## If it goes wrong
- **Black screen for 5 minutes:** hold the power button. Boot with the **old 4 GB stick**, which stays on the basic-display config. Then run:
  ```bash
  sudo sh /Volumes/OPENCORE/gpuoff
  ```
  It runs the driver's `uninstall.sh` and puts the Lexar's config back.
- **Windows is never affected:** take the stick out and it boots as before.
