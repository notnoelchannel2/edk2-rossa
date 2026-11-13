#
# Samsung Galaxy Core Prime LTE (rossa, SM-G360F / coreprimeltexx)
#
# MSM8916, 1 GB RAM, 4.5" 480x800 DSI video panel (SC7798A or HX8369B),
# Imagis IST3038 touch, 8 GB eMMC on SDC1, microSD on SDC2.
#
# Hardware details from the cm-12.1 sources (kernel/samsung/coreprimelte,
# msm8916-sec-rossa-eur-r0x.dts) and mainline msm8916-samsung-rossa.dts.
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
  FLASH_DEFINITION               = Msm8916Pkg/Devices/rossa.fdf

!include Msm8916Pkg/Msm8916Pkg.dsc

[BuildOptions.common]
  # Selects Include/Configuration/DeviceMemoryMapRossa.h
  GCC:*_*_AARCH64_CC_FLAGS = -DMSM8916_DEVICE_ROSSA

[PcdsFixedAtBuild.common]
  # 1 GB at 0x80000000. The UEFI region (stacks/HOBs) is carved from the top
  # of this, so it must not be larger than the real RAM.
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x40000000

  # Simple FrameBuffer (4.5" 480x800). PrePi points the MDP pipe here.
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferWidth|480
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferHeight|800
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferAddress|0x8e000000
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferPixelBpp|32
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleWidth|480
  gMsm8916PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleHeight|800

  # SmBios Information
  gMsm8916PkgTokenSpaceGuid.PcdDeviceVendor|"Samsung"
  gMsm8916PkgTokenSpaceGuid.PcdDeviceProduct|"Galaxy Core Prime LTE"
  gMsm8916PkgTokenSpaceGuid.PcdDeviceCodeName|"rossa"

  # SD card detect (sdc2_cd, gpio38, active low)
  gQcomTokenSpaceGuid.PcdSdCardDetectGpio|38
  gQcomTokenSpaceGuid.PcdSdCardDetectActiveLow|TRUE

  # Imagis IST3038 (legacy IST30xx protocol) on blsp_i2c5, bit-banged:
  # SDA gpio18, SCL gpio19, addr 0x50, INT gpio13, vdd_tsp_a enable gpio73
  gQcomTokenSpaceGuid.PcdTouchI2cSdaGpio|18
  gQcomTokenSpaceGuid.PcdTouchI2cSclGpio|19
  gQcomTokenSpaceGuid.PcdTouchI2cAddress|0x50
  gQcomTokenSpaceGuid.PcdTouchIntGpio|13
  gQcomTokenSpaceGuid.PcdTouchPowerEnableGpio|73
  gQcomTokenSpaceGuid.PcdTouchMaxX|480
  gQcomTokenSpaceGuid.PcdTouchMaxY|800
  gQcomTokenSpaceGuid.PcdTouchImagisLegacy|TRUE

  # Keys: Vol+ gpio107, Home gpio109 (both active low, pull-up),
  # Vol- on PM8916 RESIN, Power on PM8916 KPDPWR
  gQcomTokenSpaceGuid.PcdKeypadVolumeUpGpio|107
  gQcomTokenSpaceGuid.PcdKeypadHomeGpio|109
  gQcomTokenSpaceGuid.PcdKeypadVolumeDownResin|TRUE
  gQcomTokenSpaceGuid.PcdKeypadPowerPon|TRUE

[PcdsPatchableInModule.common]
  # 480 px is only 60 text columns, so there is no 80x25 mode: let the
  # console and ConSplitter pick the largest mode (60x42) instead.
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|0

[Components.common]
  # GraphicsConsoleDxe with small screen support (replaces MdeModulePkg's in rossa.fdf)
  Msm8916Pkg/Drivers/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  Msm8916Pkg/Drivers/Ist3038cTouchDxe/Ist3038cTouchDxe.inf
  Msm8916Pkg/Application/TouchTestApp/TouchTestApp.inf
  Msm8916Pkg/Application/J5DiagApp/J5DiagApp.inf
