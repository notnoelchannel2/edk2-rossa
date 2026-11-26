#
# Samsung Galaxy A3 2015 (a3ulte / a3lte, SM-A300F / SM-A300FU / SM-A300YZ)
#
# MSM8916, 1 GB RAM, 4.5" 540x960 Super AMOLED DSI video panel
# (Samsung S6E88A0-AMS452EF01, 2 lanes), Zinitix BT541 touch, capacitive
# Recents/Back keys (ABOV MCU, TC360 protocol), 16 GB eMMC on SDC1,
# microSD on SDC2, RT5033 charger/fuel gauge, SM5502 MUIC.
#
# Hardware details from mainline Linux (msm8916-mainline 6.12.1):
# msm8916-samsung-a3u-eur.dts / msm8916-samsung-a2015-common.dtsi, and
# lk2nd (lk2nd/device/dts/msm8916/msm8916-samsung.dts, board-id 0xCE08FF01 1).
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
  FLASH_DEFINITION               = Msm8916Pkg/Devices/a3ulte.fdf

  # DSDT with the A3 touch device (A3TS0001) and the motor on GPIO76
  DEFINE ACPI_TABLES_INF         = Msm8916Pkg/AcpiTables/AcpiTablesA3ulte.inf

!include Msm8916Pkg/Msm8916Pkg.dsc

[BuildOptions.common]
  # Selects Include/Configuration/DeviceMemoryMapA3ulte.h
  GCC:*_*_AARCH64_CC_FLAGS = -DMSM8916_DEVICE_A3ULTE

[PcdsFixedAtBuild.common]
  # 1 GB at 0x80000000. The UEFI region (stacks/HOBs) is carved from the top
  # of this, so it must not be larger than the real RAM.
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x40000000

  # Simple FrameBuffer (4.5" 540x960). The Samsung bootloader leaves the
  # AMOLED panel running in video mode (lk2nd keeps the continuous splash);
  # PrePi points the MDP pipe here, 32bpp, stride 540 * 4.
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferWidth|540
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferHeight|960
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferAddress|0x8e000000
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferPixelBpp|32
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleWidth|540
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleHeight|960

  # SmBios Information
  gMsm8916PkgTokenSpaceGuid.PcdDeviceVendor|"Samsung"
  gMsm8916PkgTokenSpaceGuid.PcdDeviceProduct|"Galaxy A3 (2015)"
  gMsm8916PkgTokenSpaceGuid.PcdDeviceCodeName|"a3ulte"

  # SD card detect (sdc2_cd, gpio38, active low)
  gQcomTokenSpaceGuid.PcdSdCardDetectGpio|38
  gQcomTokenSpaceGuid.PcdSdCardDetectActiveLow|TRUE

  # Zinitix BT541 on blsp_i2c5, bit-banged: SDA gpio18, SCL gpio19,
  # addr 0x20, INT gpio13, vcca = vdd_tsp_a (enable gpio73), vdd = PM8916 L6
  gQcomTokenSpaceGuid.PcdTouchI2cSdaGpio|18
  gQcomTokenSpaceGuid.PcdTouchI2cSclGpio|19
  gQcomTokenSpaceGuid.PcdTouchI2cAddress|0x20
  gQcomTokenSpaceGuid.PcdTouchIntGpio|13
  gQcomTokenSpaceGuid.PcdTouchPowerEnableGpio|73
  gQcomTokenSpaceGuid.PcdTouchMaxX|540
  gQcomTokenSpaceGuid.PcdTouchMaxY|960

  # Keys: Vol+ gpio107, Home gpio109 (both active low, pull-up),
  # Vol- on PM8916 RESIN, Power on PM8916 KPDPWR
  # No keyboard: the UEFI Shell (needs typing) is only listed in the boot
  # manager menu instead of being booted automatically; with nothing else
  # to boot the menu comes up (side keys work there)
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

  # Capacitive Recents (Enter) / Back (Esc) keys: i2c-gpio SDA 16 / SCL 17,
  # addr 0x20, VCC 2.8V enable gpio86, key LED (VDD) 3.3V enable gpio60
!ifndef NO_TOUCHKEYS
  gQcomTokenSpaceGuid.PcdTouchkeyI2cSdaGpio|16
  gQcomTokenSpaceGuid.PcdTouchkeyI2cSclGpio|17
  gQcomTokenSpaceGuid.PcdTouchkeyI2cAddress|0x20
  gQcomTokenSpaceGuid.PcdTouchkeyPowerGpio|86
  gQcomTokenSpaceGuid.PcdTouchkeyLedPowerGpio|60
!endif

  # Vibration motor: gpio76 enables the haptic driver IC, gpio50 is its PWM
  # input (GCC_GP2 clock in Linux), held high (100%) by PlatformPeiLib
  gQcomTokenSpaceGuid.PcdMotorEnableGpio|76
  gQcomTokenSpaceGuid.PcdMotorPwmGpio|50

  # USB host: RT5033 OTG boost (blsp_i2c6 = gpio 22/23 @0x34), SM5502 MUIC
  # (i2c-gpio 105/106 @0x25, same register layout as the SM5703/SM5504)
  gQcomTokenSpaceGuid.PcdOtgBoostI2cSdaGpio|22
  gQcomTokenSpaceGuid.PcdOtgBoostI2cSclGpio|23
  gQcomTokenSpaceGuid.PcdOtgBoostI2cAddress|0x34
  gQcomTokenSpaceGuid.PcdOtgBoostRt5033|TRUE
  gQcomTokenSpaceGuid.PcdMuicI2cSdaGpio|105
  gQcomTokenSpaceGuid.PcdMuicI2cSclGpio|106
  gQcomTokenSpaceGuid.PcdMuicI2cAddress|0x25

[PcdsFeatureFlag.common]
  # MpParkDxe swaps the MADT through the ACPI SDT protocol
  gEfiMdeModulePkgTokenSpaceGuid.PcdInstallAcpiSdtProtocol|TRUE

[PcdsPatchableInModule.common]
  # 540 px is only 67 text columns, so there is no 80x25 mode: let the
  # console and ConSplitter pick the largest mode instead.
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|0

[Components.common]
  # GraphicsConsoleDxe with small screen support (replaces MdeModulePkg's in a3ulte.fdf)
  Msm8916Pkg/Drivers/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  Msm8916Pkg/Drivers/ZinitixTouchDxe/ZinitixTouchDxe.inf
  Msm8916Pkg/Drivers/Rt5033ChargerDxe/Rt5033ChargerDxe.inf
  Msm8916Pkg/Drivers/MpParkDxe/MpParkDxe.inf
  Msm8916Pkg/Drivers/UsbHostDxe/UsbHostDxe.inf
  Msm8916Pkg/Application/TouchTestApp/TouchTestApp.inf
  Msm8916Pkg/Application/RossaDiagApp/RossaDiagApp.inf
