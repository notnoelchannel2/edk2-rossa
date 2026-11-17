#
# Samsung Galaxy Grand Prime LTE (gprimeltexx, SM-G530FZ, "fortuna" EUR)
#
# MSM8916, 1 GB RAM, 5.0" 540x960 DSI video panel (HX8389C or S6D78A0),
# Zinitix BT541 touch, 8 GB eMMC on SDC1, microSD on SDC2, RT5033 PMIC
# (charger/fuel gauge/OTG boost), SM5502 MUIC.
#
# Same board family as rossa (Core Prime LTE): touch, battery, keys, motor,
# SD and USB use the same GPIOs. Differences: panel size, touch chip,
# Samsung TZ carve-out, MUIC model, PWM backlight.
#
# Hardware details from the Galaxy-MSM8916 sources (kernel
# msm8916-sec-fortuna-eur-r0x.dts, drivers/misc/sm5502.c) and mainline
# msm8916-samsung-fortuna-common.dtsi / msm8916-samsung-grandprimelte.dts.
#
[Defines]
  PLATFORM_NAME                  = Msm8916Pkg
  PLATFORM_GUID                  = 28f1a3bf-193a-47e3-a7b9-5a435eaab2ee
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Msm8916Pkg/Devices/gprimeltexx.fdf

  # Grand Prime DSDT: Zinitix touch, sensors, GP* device IDs for the gprime-win10 drivers
  DEFINE ACPI_TABLES_INF         = Msm8916Pkg/AcpiTables/AcpiTablesGprimeltexx.inf

!include Msm8916Pkg/Msm8916Pkg.dsc

[BuildOptions.common]
  # Selects Include/Configuration/DeviceMemoryMapGprimeltexx.h
  GCC:*_*_AARCH64_CC_FLAGS = -DMSM8916_DEVICE_GPRIMELTEXX

[PcdsFixedAtBuild.common]
  # 1 GB at 0x80000000. The UEFI region (stacks/HOBs) is carved from the top
  # of this, so it must not be larger than the real RAM.
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x40000000

  # Simple FrameBuffer (5.0" 540x960). PrePi points the MDP pipe here.
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferWidth|540
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferHeight|960
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferAddress|0x8e000000
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferPixelBpp|32
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleWidth|540
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleHeight|960

  # SmBios Information
  gMsm8916PkgTokenSpaceGuid.PcdDeviceVendor|"Samsung"
  gMsm8916PkgTokenSpaceGuid.PcdDeviceProduct|"Galaxy Grand Prime LTE"
  gMsm8916PkgTokenSpaceGuid.PcdDeviceCodeName|"gprimeltexx"

  # SD card detect (sdc2_cd, gpio38, active low)
  gQcomTokenSpaceGuid.PcdSdCardDetectGpio|38
  gQcomTokenSpaceGuid.PcdSdCardDetectActiveLow|TRUE

  # Zinitix BT541 on blsp_i2c5 pins, bit-banged (Samsung does the same, i2c-gpio):
  # SDA gpio18, SCL gpio19, addr 0x20, INT gpio13, vdd_tsp_a enable gpio73
  gQcomTokenSpaceGuid.PcdTouchI2cSdaGpio|18
  gQcomTokenSpaceGuid.PcdTouchI2cSclGpio|19
  gQcomTokenSpaceGuid.PcdTouchI2cAddress|0x20
  gQcomTokenSpaceGuid.PcdTouchIntGpio|13
  gQcomTokenSpaceGuid.PcdTouchPowerEnableGpio|73
  gQcomTokenSpaceGuid.PcdTouchMaxX|540
  gQcomTokenSpaceGuid.PcdTouchMaxY|960

  # Keys: Vol+ gpio107, Home gpio109 (both active low, pull-up),
  # Vol- on PM8916 RESIN, Power on PM8916 KPDPWR
  # No keyboard: the UEFI Shell is only listed in the boot manager menu
  gMsm8916PkgTokenSpaceGuid.PcdBootToMenu|TRUE

  gQcomTokenSpaceGuid.PcdKeypadVolumeUpGpio|107
  gQcomTokenSpaceGuid.PcdKeypadHomeGpio|109
!ifdef NO_PMIC_KEYS
  # Test build: don't touch the PMIC bus (SPMI) for keys
  gQcomTokenSpaceGuid.PcdKeypadVolumeDownResin|FALSE
  gQcomTokenSpaceGuid.PcdKeypadPowerPon|FALSE
!else
  gQcomTokenSpaceGuid.PcdKeypadVolumeDownResin|TRUE
  gQcomTokenSpaceGuid.PcdKeypadPowerPon|TRUE
!endif

  # USB host: RT5033 OTG boost (I2C 22/23 @0x34, as on rossa),
  # SM5502 MUIC (I2C 2/3 @0x25). Samsung's sm5502_set_otg() writes
  # MANUAL_SW1 = 0x25 and MANUAL_SW2 = 0x00 (the SM5504 needs 0x02).
  gQcomTokenSpaceGuid.PcdOtgBoostI2cSdaGpio|22
  gQcomTokenSpaceGuid.PcdOtgBoostI2cSclGpio|23
  gQcomTokenSpaceGuid.PcdOtgBoostI2cAddress|0x34
  gQcomTokenSpaceGuid.PcdOtgBoostRt5033|TRUE
  gQcomTokenSpaceGuid.PcdMuicI2cSdaGpio|2
  gQcomTokenSpaceGuid.PcdMuicI2cSclGpio|3
  gQcomTokenSpaceGuid.PcdMuicI2cAddress|0x25
  gQcomTokenSpaceGuid.PcdMuicManualSw2Otg|0x00

[PcdsFeatureFlag.common]
  # MpParkDxe swaps the MADT through the ACPI SDT protocol
  gEfiMdeModulePkgTokenSpaceGuid.PcdInstallAcpiSdtProtocol|TRUE

[PcdsPatchableInModule.common]
  # 540 px is only 67 text columns, so there is no 80x25 mode: let the
  # console and ConSplitter pick the largest mode (67x50) instead.
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|0

[Components.common]
  # GraphicsConsoleDxe with small screen support (replaces MdeModulePkg's in gprimeltexx.fdf)
  Msm8916Pkg/Drivers/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  Msm8916Pkg/Drivers/Bt541TouchDxe/Bt541TouchDxe.inf
  Msm8916Pkg/Drivers/Rt5033ChargerDxe/Rt5033ChargerDxe.inf
  Msm8916Pkg/Drivers/MpParkDxe/MpParkDxe.inf
  Msm8916Pkg/Drivers/UsbHostDxe/UsbHostDxe.inf
  Msm8916Pkg/Application/TouchTestApp/TouchTestApp.inf
  Msm8916Pkg/Application/GprimeDiagApp/GprimeDiagApp.inf
