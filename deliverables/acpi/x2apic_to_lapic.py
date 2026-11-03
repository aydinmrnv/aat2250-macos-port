"""Rewrite an x2APIC-only MADT (ACPI "APIC" table) with classic Local APIC entries, for macOS.

usage: python3 -I x2apic_to_lapic.py APIC.aml APIC-LAPIC.aml

Why: the AAT2250 firmware lists every CPU only as a Processor Local x2APIC structure (type 9). XNU counts CPUs from
Processor Local APIC structures (type 0), finds none and stops right after ExitBootServices, before any console output
(test boot 2026-11-03, hang at "EB|LOG:EXITBS:START"; same symptom and fix on other Arrow Lake machines).

What it does, per the ACPI MADT layout (ACPI 6.5 section 5.2.12):
  - type 9  (x2APIC, 16 bytes), enabled, x2APIC ID <= 0xFE and UID <= 0xFE -> type 0 (Local APIC, 8 bytes)
    with the same ACPI Processor UID, APIC ID and flags. Firmware order is kept, so the boot CPU stays first.
  - type 9 entries that are not enabled (the firmware's 0xFFFFFFFF placeholders) are dropped.
  - type 10 (x2APIC NMI, 12 bytes) -> type 4 (Local APIC NMI, 6 bytes); UID 0xFFFFFFFF ("all") becomes 0xFF.
  - every other structure (I/O APIC, interrupt source overrides, ...) is copied unchanged.
  - header length and checksum are recomputed; everything else in the header is kept.
It refuses (exits non-zero) if any enabled CPU has an ID that does not fit in 8 bits, instead of producing a wrong table.
"""
import struct
import sys


def convert(src: bytes) -> bytes:
    if src[:4] != b"APIC":
        raise SystemExit("not an APIC (MADT) table")
    length = struct.unpack_from("<I", src, 4)[0]
    body = bytearray(src[36:44])             # Local Interrupt Controller Address + Flags
    off, n_lapic, n_dropped = 44, 0, 0
    while off + 2 <= length:
        t, l = src[off], src[off + 1]
        if l < 2 or off + l > length:
            raise SystemExit(f"malformed structure at offset {off}")
        s = src[off:off + l]
        if t == 9:
            _, x2id, flags, uid = struct.unpack_from("<HIII", s, 2)
            if not (flags & 1):
                n_dropped += 1
            elif x2id > 0xFE or uid > 0xFE:
                raise SystemExit(f"enabled CPU with x2APIC ID {x2id} / UID {uid} does not fit a Local APIC entry")
            else:
                body += struct.pack("<BBBBI", 0, 8, uid, x2id, flags)
                n_lapic += 1
        elif t == 10:
            flags, uid, lint = struct.unpack_from("<HIB", s, 2)
            body += struct.pack("<BBBHB", 4, 6, 0xFF if uid == 0xFFFFFFFF else uid, flags, lint)
        else:
            body += s
        off += l
    if not n_lapic:
        raise SystemExit("no enabled x2APIC entries found; nothing to convert")
    hdr = bytearray(src[:36])
    struct.pack_into("<I", hdr, 4, 36 + len(body))
    hdr[9] = 0
    out = hdr + body
    out[9] = (-sum(out)) & 0xFF
    assert sum(out) & 0xFF == 0
    print(f"{n_lapic} Local APIC entries written, {n_dropped} disabled x2APIC placeholders dropped, "
          f"{length} -> {len(out)} bytes")
    return bytes(out)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    data = open(sys.argv[1], "rb").read()
    open(sys.argv[2], "wb").write(convert(data))
