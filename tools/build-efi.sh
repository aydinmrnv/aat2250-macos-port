#!/bin/zsh
# build-efi.sh: assemble the installer-test OpenCore EFI for the Alienware Area-51 AAT2250. Run it on a Mac.
#
#   tools/build-efi.sh <output-dir>
#
# What it does:
#   - Downloads the pinned releases listed in deliverables/DECISIONS.md and refuses any zip whose SHA-256
#     does not match.
#   - Copies in this repo's ACPI tables, config and the VMDProbe / VMDBridge / USB4Probe kexts.
#   - Generates SMBIOS (MacPro7,1 serial and MLB with macserial, a UUID, a random ROM) into the output
#     config only. Never commit or share that output config.
# It touches no disk except <output-dir>, and it needs no sudo.
set -eu
setopt null_glob

REPO=${0:A:h:h}
OUT=${1:?usage: tools/build-efi.sh <output-dir>}
[[ -e $OUT ]] && { echo "refusing: $OUT already exists"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# name|url|sha256
PINS=(
  "OpenCore|https://github.com/acidanthera/OpenCorePkg/releases/download/1.0.8/OpenCore-1.0.8-RELEASE.zip|2011e8b7216ecb2645d97ea710df965947edd406846e92050b9bc4190be6d27b"
  "Lilu|https://github.com/acidanthera/Lilu/releases/download/1.7.2/Lilu-1.7.2-RELEASE.zip|53967d7dcfaab01023a33df2e969a89522f13d6654a6a56ac4711b62dabf3ab8"
  "VirtualSMC|https://github.com/acidanthera/VirtualSMC/releases/download/1.3.8/VirtualSMC-1.3.8-RELEASE.zip|2e29a8aaf91b4eb0bcdf5913ed17e70f5cf76531122acd4132eff7813a54de8b"
  "RestrictEvents|https://github.com/acidanthera/RestrictEvents/releases/download/1.1.6/RestrictEvents-1.1.6-RELEASE.zip|98170dfae195ddd28b5d95e3f040125a13ca783bcb9bd1e5b8c588e217b14ee6"
  "CpuTopologyRebuild|https://github.com/b00t0x/CpuTopologyRebuild/releases/download/2.0.2/CpuTopologyRebuild-2.0.2-RELEASE.zip|c25d07464d2c9adf120dbf13a50321ff1bdb273290a5e82235eef4740db7b911"
  "LucyRTL8125Ethernet|https://github.com/Mieze/LucyRTL8125Ethernet/releases/download/v1.2.4/LucyRTL8125Ethernet-V1.2.4.zip|a4e24a0246fff0bc059be4b832b7e4c800261364868632df2f38a2bf998e3956"
  "BrcmPatchRAM|https://github.com/acidanthera/BrcmPatchRAM/releases/download/2.7.2/BrcmPatchRAM-2.7.2-RELEASE.zip|e1c1c55347526d031a8ae2fdd1f52efa3019161e497fb38e1cfa809752f8af21"
  "IntelBluetooth|https://github.com/OpenIntelWireless/IntelBluetoothFirmware/releases/download/v2.4.0/IntelBluetooth-v2.4.0.zip|78418daa11f620012fc53ba68cbd643c2642a45f2f96c594fd01aaf2bc68fe2b"
  "USBToolBox|https://github.com/USBToolBox/kext/releases/download/1.2.0/USBToolBox-1.2.0-RELEASE.zip|c315a3a5acfd496dd97d0d19b4fbd1d487103d2fd541c5651583d4c9cebcfe07"
)

for p in $PINS; do
  name=${p%%|*}; rest=${p#*|}; url=${rest%%|*}; sum=${rest#*|}
  echo "fetch $name"
  curl -fsSL -o "$WORK/$name.zip" "$url"
  got=$(shasum -a 256 "$WORK/$name.zip" | cut -d' ' -f1)
  [[ $got == $sum ]] || { echo "SHA-256 MISMATCH for $name: $got"; exit 1; }
  mkdir -p "$WORK/x/$name" && ditto -x -k "$WORK/$name.zip" "$WORK/x/$name"
done

OC=$WORK/x/OpenCore/X64/EFI
mkdir -p "$OUT/EFI/BOOT" "$OUT/EFI/OC/ACPI" "$OUT/EFI/OC/Drivers" "$OUT/EFI/OC/Kexts" "$OUT/EFI/OC/Tools"
cp "$OC/BOOT/BOOTx64.efi" "$OUT/EFI/BOOT/"
# Hide the stick's own EFI/BOOT entry from the picker, so the recovery dmg is the only entry and boots at
# timeout even if the keyboard does not work in OpenCore (first test boot, 2026-11-03).
printf 'Disabled' > "$OUT/EFI/BOOT/.contentVisibility"
cp "$OC/OC/OpenCore.efi" "$OUT/EFI/OC/"
for d in OpenRuntime OpenHfsPlus ResetNvramEntry; do cp "$OC/OC/Drivers/$d.efi" "$OUT/EFI/OC/Drivers/"; done
cp "$REPO"/deliverables/acpi/*.aml "$OUT/EFI/OC/ACPI/"

kext() {   # copy the first <name>.kext found in an extracted release (skips dSYM bundles)
  local src=$(find "$WORK/x/$1" -name "$2.kext" -not -path '*dSYM*' -prune | head -1)
  [[ -n $src ]] || { echo "missing $2.kext in $1"; exit 1; }
  ditto "$src" "$OUT/EFI/OC/Kexts/$2.kext"
}
kext Lilu Lilu
kext VirtualSMC VirtualSMC
kext VirtualSMC SMCProcessor
kext RestrictEvents RestrictEvents
kext CpuTopologyRebuild CpuTopologyRebuild
kext LucyRTL8125Ethernet LucyRTL8125Ethernet
kext BrcmPatchRAM BlueToolFixup
kext IntelBluetooth IntelBluetoothFirmware
kext IntelBluetooth IntelBTPatcher
kext USBToolBox USBToolBox
kext USBToolBox UTBDefault
for k in kexts/MCEReporterDisabler vmd-driver/dist/VMDProbe vmd-driver/dist/VMDBridge usb4-driver/dist/USB4Probe; do
  ditto "$REPO/$k.kext" "$OUT/EFI/OC/Kexts/${k:t}.kext"
done

CFG=$OUT/EFI/OC/config.plist
cp "$REPO/efi/config.plist" "$CFG"
MS=$WORK/x/OpenCore/Utilities/macserial/macserial
chmod +x "$MS"
read -r SERIAL MLB <<< "$("$MS" -m MacPro7,1 -n 1 | tr -d '|' | awk '{print $1, $2}')"
[[ -n $SERIAL && -n $MLB ]] || { echo "macserial failed"; exit 1; }
plutil -replace PlatformInfo.Generic.SystemSerialNumber -string "$SERIAL" "$CFG"
plutil -replace PlatformInfo.Generic.MLB -string "$MLB" "$CFG"
plutil -replace PlatformInfo.Generic.SystemUUID -string "$(uuidgen)" "$CFG"
plutil -replace PlatformInfo.Generic.ROM -data "$(head -c 6 /dev/urandom | base64)" "$CFG"

OCV=$WORK/x/OpenCore/Utilities/ocvalidate/ocvalidate
chmod +x "$OCV"
"$OCV" "$CFG"
echo "EFI ready at $OUT/EFI (contains your generated SMBIOS: keep it private)"
