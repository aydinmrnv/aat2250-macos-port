#!/bin/zsh
# get-recovery.sh: download the macOS Tahoe recovery image (BaseSystem.dmg + chunklist) from Apple with OpenCore's
# macrecovery.py, taken from the pinned, hash-checked OpenCore 1.0.8 release. Run it on a Mac.
#
#   tools/get-recovery.sh <output-dir>      ->  <output-dir>/com.apple.recovery.boot/
set -eu
OUT=${1:?usage: tools/get-recovery.sh <output-dir>}
[[ -e $OUT ]] && { echo "refusing: $OUT already exists"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

URL=https://github.com/acidanthera/OpenCorePkg/releases/download/1.0.8/OpenCore-1.0.8-RELEASE.zip
SUM=2011e8b7216ecb2645d97ea710df965947edd406846e92050b9bc4190be6d27b
curl -fsSL -o "$WORK/oc.zip" "$URL"
[[ $(shasum -a 256 "$WORK/oc.zip" | cut -d' ' -f1) == $SUM ]] || { echo "SHA-256 MISMATCH for OpenCore"; exit 1; }
ditto -x -k "$WORK/oc.zip" "$WORK/oc"

mkdir -p "$OUT/com.apple.recovery.boot"
# Board ID of MacPro7,1, the SMBIOS this config uses; -os latest gives the current release (Tahoe).
python3 -I "$WORK/oc/Utilities/macrecovery/macrecovery.py" -b Mac-CFF7D910A743CAAF -m 00000000000000000 \
  -os latest -o "$OUT/com.apple.recovery.boot" download
ls -l "$OUT/com.apple.recovery.boot"
