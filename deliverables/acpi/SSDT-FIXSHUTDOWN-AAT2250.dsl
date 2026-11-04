/*
 * SSDT-FIXSHUTDOWN for Alienware Area-51 AAT2250 (Arrow Lake-S, Dell DSDT).
 *
 * Problem (expected, not yet seen here): Apple menu > Shut Down comes back as a restart. On Intel desktops and laptops an
 * xHCI controller keeps PME_En set when the platform enters S5, signals PME, and the PCH powers the machine straight
 * back on. Windows and Linux clear it; macOS does not. Seen and fixed on an Arrow Lake Legion 5 running Tahoe
 * (watchthelight-labs/WTLLegionOpenCore, SSDT-FIXSHUTDOWN); the pattern is RehabMan's FixShutdown from Dortania's
 * post-install guide. This file is written for the Dell tables.
 *
 * Dell specifics:
 *   - \_SB.PC02.XHCI (PCH xHCI 8086:7F6E): the DSDT's XPRT field has no PMEE, so this table maps PMCSR itself.
 *     Intel PCH xHCI: Power Management capability at 0x70, PMCSR at 0x74, PME_En = bit 8 = byte 0x75 bit 0.
 *   - \_SB.PC00.TXHC (TCSS xHCI 8086:7EC0): PMEE is defined in Dell's TcssSsdt; used through External.
 * Needs the OpenCore ACPI > Patch renaming the DSDT Method (_PTS, 1) to ZPTS (find "_PTS" + ArgCount 0x01, unique).
 * Darwin and S5 only; the firmware _PTS always runs first, unchanged.
 */
DefinitionBlock ("", "SSDT", 2, "AAT250", "FIXSHUT", 0x00001000)
{
    External (ZPTS, MethodObj)                      // the renamed firmware _PTS, 1 argument
    External (_SB_.PC02.XHCI, DeviceObj)
    External (_SB_.PC00.TXHC, DeviceObj)
    External (_SB_.PC00.TXHC.PMEE, FieldUnitObj)

    Scope (\_SB.PC02.XHCI)
    {
        OperationRegion (XPMC, PCI_Config, 0x74, 0x04)
        Field (XPMC, ByteAcc, NoLock, Preserve)
        {
            Offset (0x01),
            XPME,   1                               // PMCSR bit 8, PME_En
        }
    }

    Method (_PTS, 1, NotSerialized)  // _PTS: Prepare To Sleep
    {
        ZPTS (Arg0)
        If (_OSI ("Darwin"))
        {
            If ((Arg0 == 0x05))
            {
                \_SB.PC02.XHCI.XPME = Zero
                If (CondRefOf (\_SB.PC00.TXHC.PMEE))
                {
                    \_SB.PC00.TXHC.PMEE = Zero
                }
            }
        }
    }
}
