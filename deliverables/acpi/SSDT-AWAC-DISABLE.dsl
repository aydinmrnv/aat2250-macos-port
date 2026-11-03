/*
 * SSDT-AWAC-DISABLE for Alienware Area-51 AAT2250. Identical logic to OpenCorePkg 1.0.8 sample (8d8d690c).
 * Machine facts (this machine's DSDT): two ACPI000E (AWAC) devices whose _STA returns 0x0F only while
 * STAS == 0 (STAS is a GNVS field); the legacy RTC (PNP0B00) has no _STA, so it is always present.
 * Windows currently shows both ACPI000E and PNP0B00 active. Setting STAS = 1 under Darwin hides AWAC and
 * leaves the legacy RTC that macOS supports. The DSDT has no root \_INI, so this one does not collide.
 */
DefinitionBlock ("", "SSDT", 2, "ACDT", "NOAWAC", 0x00000000)
{
    External (STAS, IntObj)

    Scope (\)
    {
        Method (_INI, 0, NotSerialized)
        {
            If (_OSI ("Darwin"))
            {
                STAS = One
            }
        }
    }
}
