DefinitionBlock("DSDT_ROSSA.AML", "DSDT", 0x02, "QCOMM ", "MSM8916 ", 3)
{
	/* CPU */
	Scope(\_SB_)
	{
		/*
		 * 4 CPU cores. MpParkDxe powers up CPU1-3, parks them for the MP
		 * Parking Protocol and puts only the cores that arrived in the MADT;
		 * it leaves that same mask (bit n = CPUn) at 0x80300F00 for _STA.
		 */
		OperationRegion (MPPK, SystemMemory, 0x80300F00, 4)
		Field (MPPK, DWordAcc, NoLock, Preserve)
		{
			CPUM, 32
		}

		Device(CPU0) 
		{
			Name(_HID, "ACPI0007")
			Name(_UID, 0)
		}
		Device(CPU1) 
		{
			Name(_HID, "ACPI0007")
			Name(_UID, 1)
			Method(_STA, 0, NotSerialized)
			{
				If (And (CPUM, 2)) { Return (0x0F) }
				Return (Zero)
			}
		}
		Device(CPU2) 
		{
			Name(_HID, "ACPI0007")
			Name(_UID, 2)
			Method(_STA, 0, NotSerialized)
			{
				If (And (CPUM, 4)) { Return (0x0F) }
				Return (Zero)
			}
		}
		Device(CPU3) 
		{
			Name(_HID, "ACPI0007")
			Name(_UID, 3)
			Method(_STA, 0, NotSerialized)
			{
				If (And (CPUM, 8)) { Return (0x0F) }
				Return (Zero)
			}
		}
	}

	/* EMMC */
	Device (SDC1)
	{
		Name (_HID, "QCOM2466")
		Name (_CID, "ACPI\QCOM2466")
		Name (_UID, Zero)  // _UID: Unique ID
		Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute

		Method (_CRS, 0, NotSerialized)  // _CRS: Current Resource Settings
		{
			Name (RBUF, ResourceTemplate ()
			{
				Memory32Fixed (ReadWrite,
				0x07824900,	 // Address Base
				0x00000200,	 // Address Length
				)
				Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, )
				{
					155,
				}
			})
			Return (RBUF) /* \_SB_.SDC1._CRS.RBUF */
		}

		Device (EMMC)
		{
			Method (_ADR, 0, NotSerialized)  // _ADR: Address
			{
				Return (0x08)
			}

			Method (_RMV, 0, NotSerialized)  // _RMV: Removal Status
			{
				Return (Zero)
			}
		}

		Method (_DIS, 0, NotSerialized)  // _DIS: Disable Device
		{
		}

		/*
		 * The eMMC holds the phone's Android/bootloader partitions: keep it
		 * invisible to Windows (no sdbus, nothing for diskpart to touch).
		 */
		Method (_STA, 0, NotSerialized)  // _STA: Status
		{
			Return (Zero)
		}
	}

	Device (SDC2)
	{
		Name (_HID, "QCOM2466")
		Name (_CID, "ACPI\QCOM2466")
		Name (_UID, 1)  // _UID: Unique ID
		Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute

		Method (_CRS, 0, NotSerialized)  // _CRS: Current Resource Settings
		{
			Name (RBUF, ResourceTemplate ()
			{
				Memory32Fixed (ReadWrite,
					0x07864900,	 // Address Base
					0x00000200,	 // Address Length
				)
				Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, )
				{
					157,
				}
			})
			Return (RBUF) /* \_SB_.SDC1._CRS.RBUF */
		}

		/*
		 * The J5 wires SD card detect to GPIO 38, not to the SDHCI card
		 * detect line, and Windows has no TLMM GPIO driver. Declaring the
		 * card embedded (_RMV 0) makes sdbus probe it without card detect;
		 * the card must be in place at boot.
		 */
		Device (SDMM)
		{
			Name (_ADR, Zero)  // slot 0, function 0
			Method (_RMV, 0, NotSerialized)  // _RMV: Removal Status
			{
				Return (Zero)
			}
		}

		Method (_DIS, 0, NotSerialized)  // _DIS: Disable Device
		{
		}

		Method (_STA, 0, NotSerialized)  // _STA: Status
		{
			Return (0x0F)
		}
	}
	/*
	 * USB 2.0 OTG controller (ChipIdea, usb@78d9000) as a plain EHCI host.
	 * UsbHostDxe brings up clocks/PHY and puts it in host mode before boot.
	 * The EHCI register set starts at the capability registers (+0x100).
	 */
	Device (USB0)
	{
		Name (_HID, "QCOM0BB0")
		Name (_CID, "PNP0D20")
		Name (_UID, Zero)
		Name (_CCA, Zero)

		OperationRegion (UCAP, SystemMemory, 0x078D9108, 4)
		Field (UCAP, DWordAcc, NoLock, Preserve)
		{
			HCCP, 32	// HCCPARAMS
		}

		/*
		 * Integrated TT flavour for usbehci: 2 = speed in HOSTPC (ChipIdea
		 * cores with LPM, HCCPARAMS bit 17), 1 = speed in PORTSC.
		 */
		Method (_HRV, 0, NotSerialized)
		{
			If (And (HCCP, 0x20000))
			{
				Return (2)
			}
			Return (1)
		}

		Name (_CRS, ResourceTemplate ()
		{
			Memory32Fixed (ReadWrite, 0x078D9100, 0x00000200)
			Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, ) { 166 }
		})

		Method (_STA, 0, NotSerialized)
		{
			Return (0x0F)
		}
	}
	/*
	 * Imagis IST3038 touchscreen (Galaxy Core Prime LTE, rossa). Windows has
	 * no I2C/GPIO controller drivers for this SoC, so rossatouch.sys drives
	 * the chip itself by bit-banging I2C on the TLMM GPIO pages listed here,
	 * in this order: GPIO13 (INT), GPIO18 (SDA), GPIO19 (SCL),
	 * GPIO73 (vdd_tsp_a enable). The firmware has already powered the chip.
	 */
	Device (TSC0)
	{
		Name (_HID, "RSTS0001")
		Name (_UID, Zero)
		Name (_CRS, ResourceTemplate ()
		{
			Memory32Fixed (ReadWrite, 0x0100D000, 0x00001000)
			Memory32Fixed (ReadWrite, 0x01012000, 0x00001000)
			Memory32Fixed (ReadWrite, 0x01013000, 0x00001000)
			Memory32Fixed (ReadWrite, 0x01049000, 0x00001000)
		})
		Method (_STA, 0, NotSerialized)
		{
			Return (0x0F)
		}
	}

	/*
	 * RT5033 charger + fuel gauge (rossabatt.sys, rossa-winpe). Bit-banged
	 * I2C on the TLMM GPIO pages listed here, in this order:
	 * GPIO22 (charger SDA), GPIO23 (charger SCL), GPIO14 (gauge SDA),
	 * GPIO15 (gauge SCL). Custom _HID so the ACPI battery driver
	 * (cmbatt, PNP0C0A) does not bind.
	 */
	Device (BAT0)
	{
		Name (_HID, "RSBT0001")
		Name (_UID, Zero)
		Name (_CRS, ResourceTemplate ()
		{
			Memory32Fixed (ReadWrite, 0x01016000, 0x00001000)
			Memory32Fixed (ReadWrite, 0x01017000, 0x00001000)
			Memory32Fixed (ReadWrite, 0x0100E000, 0x00001000)
			Memory32Fixed (ReadWrite, 0x0100F000, 0x00001000)
		})
		Method (_STA, 0, NotSerialized)
		{
			Return (0x0F)
		}
	}
}
