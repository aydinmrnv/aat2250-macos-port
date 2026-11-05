"""Derive the post-install OpenCore config (RTX 5090 driver) from the installer-test config that boots.

  python3 -I tools/make-postinstall-config.py IN.plist OUT.plist

Use it once macOS is installed on a disk and you are about to install the NVIDIA driver
(nullmoth/nvidia-macos-driver). Everything that made the installer config boot stays as it is; only what the driver
needs is added. Sources: the driver's README (SIP 0x0A43, AMFI off, ipc_control_port_options=0), and two Arrow Lake
Tahoe configs that run it (a Z890 desktop with an RTX 4070 Super, a Legion 5 15IAX10 with an RTX 5070 Laptop).

What it changes:
  - boot-args: + amfi_get_out_of_my_way=0x1 amfi=0x80 ipc_control_port_options=0 (AMFI relaxed for the driver's
    ad hoc signed Metal plugin) and the driver's own nvfb=1 nvaccel=1 nvfbheads=4 -nvkmsnosmooth.
  - Kernel > Block com.apple.iokit.IONDRVSupport (Apple's dumb framebuffer must not hold the GPU the driver takes).
  - Kernel > Quirks > DisableIoMapperMapping = true (with VT-d hidden, the NVIDIA driver otherwise sees IOMMU-mapped
    BARs and refuses the card; Legion 5 finding).
  - Kernel > Add: AMFIPass.kext enabled if an entry exists (lets WindowServer load the plugin).
  - Misc > Debug: logging back to errors only, so boots are fast again.
  - csr-active-config is checked to be 0x0A43 (already set in the installer config).
It does not install the driver itself: that is the driver's own package, reviewed first (deliverables/a4-security).
"""
import plistlib
import sys

NV_GUID = "7C436110-AB2A-4BBB-A880-FE41995C9F82"
ADD_ARGS = ["amfi_get_out_of_my_way=0x1", "amfi=0x80", "ipc_control_port_options=0",
            "nvfb=1", "nvaccel=1", "nvfbheads=4", "-nvkmsnosmooth"]


def main(src, dst):
    c = plistlib.load(open(src, "rb"))
    nv = c["NVRAM"]["Add"][NV_GUID]
    args = nv["boot-args"].split()
    nv["boot-args"] = " ".join(args + [a for a in ADD_ARGS if a not in args])
    csr = nv.get("csr-active-config", b"")
    if csr[:2] != b"\x43\x0a":
        raise SystemExit(f"csr-active-config is {csr.hex()}, expected 430a0000 (0x0A43)")

    block = c["Kernel"].setdefault("Block", [])
    entry = next((b for b in block if b.get("Identifier") == "com.apple.iokit.IONDRVSupport"), None)
    if entry is None:
        entry = {"Arch": "Any", "Comment": "", "Identifier": "com.apple.iokit.IONDRVSupport", "MaxKernel": "",
                 "MinKernel": "", "Strategy": "Exclude"}
        block.append(entry)
    entry.update({"Enabled": True, "Strategy": "Exclude", "Comment": "NVIDIA driver owns the GPU; no IONDRV framebuffer on it (driver README: Exclude)"})

    c["Kernel"]["Quirks"]["DisableIoMapperMapping"] = True
    amfi = [k for k in c["Kernel"]["Add"] if k["BundlePath"] == "AMFIPass.kext"]
    for k in amfi:
        k["Enabled"] = True
    c["Misc"]["Debug"]["DisplayLevel"] = 0x80000002
    plistlib.dump(c, open(dst, "wb"))
    print("boot-args:", nv["boot-args"])
    print("IONDRVSupport blocked; DisableIoMapperMapping on;",
          "AMFIPass enabled" if amfi else "NOTE: no AMFIPass.kext entry, add it (pinned, reviewed) before using this")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
