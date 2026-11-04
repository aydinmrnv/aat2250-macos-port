r"""AAT2250 (Dell, Arrow Lake-S) DSDT byte patches for OpenCore ACPI > Patch, and a tester.

  python3 -I dell_dsdt_patches.py DSDT.aml [DSDT-patched.aml]   # checks every find is unique, applies, re-checksums
  python3 -I dell_dsdt_patches.py --plist                         # prints the OpenCore entries (used by the config tool)

Why (test boots 2026-11-03): the kernel froze right after `_DSM ... AE_NOT_FOUND` errors on the USB root hubs. The
DSDT is Intel Arrow Lake reference code with three constructs that macOS's ACPICA (20160930, inside
AppleACPIPlatform) handles differently from Windows and current ACPICA:

1. Module-level `If (PCHA != Zero)` blocks. ACPICA 20160930 defers module-level code until after the table load, so
   `Device (PC02)` (the PCH PCIe root, where the keyboard's xHCI lives) and the root-level block with `MNVB` and
   `Scope (_SB.PC02)` do not exist when the rest of the DSDT resolves `Scope (_SB.PC02...)`.
   Fix: turn each `If (PCHA != Zero)` into a `Scope` of the same byte length (PCHA is non-zero on this desktop: the PCH
   is present and Windows shows PC02).
2. `Name (X, Zero)` followed by a bare `Store` in a Device body. ACPICA 20160930 never runs a bare Store there, so
   `SBRG` (P2SB base: every sideband register access, including the xHCI `_PS0`/`_PS3`, goes to address 0) and
   `PU2C`/`PU3C` (USB port counts: every `If (One <= PU2C) { Device (HSxx) }` is skipped) stay 0.
   Fix: replace the Name + Store pair with a `Method` returning the source value, padded with `Noop`.

The same constructs, the same failure and the same fix were found independently on a Lenovo Legion 5 15IAX10
(Arrow Lake-HX; watchthelight-labs/WTLLegionOpenCore scripts/dsdt_patches.py, ISC). This file is our own, written
for the Dell tables: the If patches locate their PkgLength in the Dell AML instead of hard-coding it, and the
PC00.XHCI block (\-sourced) exists only on this desktop.
"""
import re
import sys

NOOP = b"\xA3"


def name(s):
    return s.encode("ascii")


def method_returning(var, src_prefix):
    """Method (VAR, 0) { Return (<prefix>VAR) }, AML. src_prefix is b'\\' or b'^^' (a Method adds a scope level)."""
    body = name(var) + b"\x00" + b"\xA4" + src_prefix + name(var)
    pkg = 1 + len(body)
    assert pkg < 0x40
    return b"\x14" + bytes([pkg]) + body


def if_pcha_to_scope(dsdt, follow, scope_name, comment):
    """If (PCHA != Zero) { <follow>... } -> Scope (<scope_name>) { Noop... <follow>... }, same length."""
    pred = b"\x92\x93" + name("PCHA") + b"\x00"                       # LNot (LEqual (PCHA, Zero))
    # PkgLength: 1 byte (0x00-0x3F) or a lead byte with 1-3 follow bytes
    rx = re.compile(re.escape(b"\xA0") + rb"((?:[\x00-\x3F])|(?:[\x40-\x7F].)|(?:[\x80-\xBF]..)|(?:[\xC0-\xFF]...))"
                    + re.escape(pred + follow), re.S)
    hits = list(rx.finditer(dsdt))
    if len(hits) != 1:
        raise SystemExit(f"{comment}: pattern occurs {len(hits)} times")
    m = hits[0]
    pkglen = m.group(1)
    find = b"\xA0" + pkglen + pred + follow
    filler = scope_name + NOOP * (len(pred) - len(scope_name))
    assert len(filler) == len(pred)
    return comment, find, b"\x10" + pkglen + filler + follow


def name_store_block(names_and_inits, stores, comment):
    """A run of Names then bare Stores in one Device body -> the non-target Names kept, Methods for the targets."""
    find = b"".join(names_and_inits) + b"".join(s for _, _, s in stores)
    keep = [n for n in names_and_inits if not any(n.startswith(b"\x08" + name(v)) for v, _, _ in stores)]
    repl = b"".join(keep) + b"".join(method_returning(v, p) for v, p, _ in stores)
    if len(repl) > len(find):
        raise SystemExit(f"{comment}: replacement longer than original")
    return comment, find, repl + NOOP * (len(find) - len(repl))


def nm(var, init):        # Name (VAR, init)
    return b"\x08" + name(var) + init


def store(src_prefix, var):   # VAR = <prefix>VAR   (Store (<prefix>VAR, VAR))
    return b"\x70" + src_prefix + name(var) + name(var)


def interleaved_pairs(vars_, comment):
    """\\_SB.PGPI: Name (X, Zero); X = ^PC02.X; ... (pairs back to back) -> Method (X) { Return (^^PC02.X) } each.
    ^PC02.X is a DualNamePath (2E PC02 X); inside the Method it needs one more parent prefix."""
    find = b"".join(nm(v, b"\x00") + b"\x70\x5E\x2E" + name("PC02") + name(v) + name(v) for v in vars_)
    repl = b""
    for v in vars_:
        body = name(v) + b"\x00" + b"\xA4" + b"\x5E\x5E\x2E" + name("PC02") + name(v)
        repl += b"\x14" + bytes([1 + len(body)]) + body
    if len(repl) > len(find):
        raise SystemExit(f"{comment}: replacement longer than original")
    return comment, find, repl + NOOP * (len(find) - len(repl))


def build(dsdt):
    zero = b"\x00"
    p = [
        if_pcha_to_scope(dsdt, b"\x5B\x82", name("\\_SB_"),
                         "If (PCHA != Zero) { Device (PC02) } -> Scope (\\_SB)"),
        if_pcha_to_scope(dsdt, b"\x08" + name("MNVB"), b"\\\x00",
                         "If (PCHA != Zero) { MNVB, Scope (_SB.PC02) } -> Scope (\\)"),
        name_store_block([nm("SBRG", zero)], [("SBRG", b"\\", store(b"\\", "SBRG"))],
                         "PC00.SBSC: Name SBRG + SBRG = \\SBRG -> Method (SBRG)"),
        name_store_block([nm("SBRG", zero)], [("SBRG", b"^^", store(b"^", "SBRG"))],
                         "PC02.SBPC: Name SBRG + SBRG = ^SBRG -> Method (SBRG)"),
        name_store_block([nm("DUWS", zero), nm("PU2C", zero), nm("PU3C", zero), nm("AHWA", b"\x01"),
                          nm("PPID", b"\x0A\x3A")],
                         [("DUWS", b"\\", store(b"\\", "DUWS")), ("PU2C", b"\\", store(b"\\", "PU2C")),
                          ("PU3C", b"\\", store(b"\\", "PU3C"))],
                         "PC00.XHCI (PPID 0x3A): DUWS/PU2C/PU3C stores -> Methods"),
        name_store_block([nm("DUWS", zero), nm("PU2C", zero), nm("PU3C", zero), nm("AHWA", b"\x01"),
                          nm("PPID", b"\x0A\x4F")],
                         [("DUWS", b"\\", store(b"\\", "DUWS")), ("PU2C", b"^^", store(b"^", "PU2C")),
                          ("PU3C", b"^^", store(b"^", "PU3C"))],
                         "PC02.XHCI (PPID 0x4F): DUWS/PU2C/PU3C stores -> Methods"),
        interleaved_pairs(["SBRG", "GEI0", "GEI1", "GEI2", "GED0", "GED1", "GED2"],
                          "\\_SB.PGPI (GPIO, INTC1084): SBRG/GEI*/GED* = ^PC02.* -> Methods"),
    ]
    for comment, f, r in p:
        assert len(f) == len(r), comment
        n = dsdt.count(f)
        if n != 1:
            raise SystemExit(f"{comment}: find occurs {n} times")
    return p


def opencore_entries(dsdt):
    return [{"Base": "", "BaseSkip": 0, "Comment": c, "Count": 1, "Enabled": True, "Find": f, "Limit": 0,
             "Mask": b"", "OemTableId": b"", "Replace": r, "ReplaceMask": b"", "Skip": 0, "TableLength": 0,
             "TableSignature": b"DSDT"} for c, f, r in build(dsdt)]


def main():
    d = bytearray(open(sys.argv[1], "rb").read())
    for comment, f, r in build(bytes(d)):
        i = d.find(f)
        d[i:i + len(f)] = r
        print(f"{comment}: {len(f)} bytes at {i:#x}")
    d[9] = 0
    d[9] = (-sum(d)) & 0xFF
    if len(sys.argv) > 2:
        open(sys.argv[2], "wb").write(d)


if __name__ == "__main__":
    main()
