# Hardware verification (read-only, Windows CIM/PnP, 2026-11-02)
System: Alienware Area-51 AAT2250, board 02JGX1 A01, BIOS 1.20.0 (2026-07-29), UEFI. RAM 68135153664 B (~63.5 GiB visible).
CPU: Intel Core Ultra 9 285K, 24C/24T.
PCI (VEN:DEV SUBSYS):
- 10DE:2B85 53051028 NVIDIA GeForce RTX 5090 (Dell-subsystem 1028:5305); 10DE:22E8 its HDA function
- 8086:7D67 0CD61028 Intel Graphics (ACTIVE: drives a 3840x2160@59 display)
- 10EC:3000 0CD61028 Killer E3100G 2.5GbE
- 8086:272B 17741A56 Killer BE1750x (BE200NGW) Wi-Fi 7
- 8086:7F50 HDA controller; 8086:AD0B VMD controller; 8086:09AB "RST VMD Managed Controller"
- USB: 8086:7EC0, 8086:7F6E (xHCI 1.20), 8086:15F0 (xHCI behind 8086:15EF switch, likely USB4/TB path), 8086:7EC2 USB4 host router
- Other: 7F04 eSPI LPC, 7F23 SMBus, 7F68 MEI, AD1D NPU, AE4C GNA, AD0D PMT
USB: 8087:0036 Intel BT; 0BDA:402E Realtek USB Audio (composite); 0BDA:8153 Realtek USB GbE; Realtek hubs 0BDA:0413/0487/5413/5487.
Displays (Windows): RTX 5090 3840x2160@120; Intel iGPU 3840x2160@59. Monitors: DELL S3225QS, 2x LG Ultra HD. Parsec virtual display adapter installed.
Disks: 0 = SK hynix PCB01 SED 2048GB NVMe (Windows; EFI p1, MSR, C: 1880 GB, 3 recovery) -- behind VMD; 1 = WD60EFZX 6TB SATA (E:); 2 = Samsung 860 EVO 500GB SATA (recovery, ESP, MSR, D:, recovery). No blank disk.
Security: Secure Boot registry UEFISecureBootEnabled=1 (direct firmware query denied, non-admin). VBS status=2 running, services {2}=HVCI. BitLocker: query denied (non-admin) -> UNKNOWN. No recovery key read.
WSL distros seen: NVIDIA-Workbench, docker-desktop (Ubuntu-24.04 listing truncated by UTF-16 output; memory says vhdx on E:\WSL2-Distros).
