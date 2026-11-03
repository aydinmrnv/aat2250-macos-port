/*
 * SSDT-EC-USBX for Alienware Area-51 AAT2250. Based on OpenCorePkg 1.0.8 SSDT-EC-USBX.dsl (8d8d690c).
 * Machine facts (this machine's DSDT):
 *   - Real EC: \_SB.PC00.LPCB.ECDV, _HID PNP0C09, _STA is a Method that always returns 0x0F.
 *     Upstream: "AppleACPIEC kext must NOT load on desktops", so ECDV is hidden from Darwin only.
 *     That REQUIRES the OpenCore ACPI->Patch renaming ECDV._STA to XSTA (see README), otherwise
 *     this table defines a duplicate _STA and fails to load.
 *   - No USBX in firmware. USBX values are the upstream defaults, not Dell-specific (UNVERIFIED).
 * Non-Darwin behaviour is unchanged (ECDV returns 0x0F; EC and USBX return 0).
 * HYPOTHESIS: LPCB sits under PC00 in ACPI while the eSPI device is PCIROOT(80)/PCI(1F00) (PC02);
 * check on first boot that \_SB.PC00.LPCB.EC appears in the IOACPIPlane.
 */
DefinitionBlock ("", "SSDT", 2, "ACDT", "SsdtEC", 0x00001000)
{
    External (_SB_.PC00.LPCB, DeviceObj)
    External (_SB_.PC00.LPCB.ECDV, DeviceObj)
    External (_SB_.PC00.LPCB.ECDV.XSTA, MethodObj)

    Scope (\_SB.PC00.LPCB.ECDV)
    {
        Method (_STA, 0, NotSerialized)
        {
            If (_OSI ("Darwin"))
            {
                Return (Zero)
            }
            Else
            {
                Return (\_SB.PC00.LPCB.ECDV.XSTA ())
            }
        }
    }

    Scope (\_SB)
    {
        Device (USBX)
        {
            Name (_ADR, Zero)
            Method (_DSM, 4, NotSerialized)
            {
                If ((Arg2 == Zero))
                {
                    Return (Buffer (One) { 0x03 })
                }

                Return (Package (0x08)
                {
                    "kUSBSleepPowerSupply", 0x13EC,
                    "kUSBSleepPortCurrentLimit", 0x0834,
                    "kUSBWakePowerSupply", 0x13EC,
                    "kUSBWakePortCurrentLimit", 0x0834
                })
            }

            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Else { Return (Zero) }
            }
        }
    }

    Scope (\_SB.PC00.LPCB)
    {
        Device (EC)
        {
            Name (_HID, "ACID0001")
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Else { Return (Zero) }
            }
        }
    }
}
