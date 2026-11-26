# EDK2 for the Galaxy Core Prime (SM-G360F / rossa, coreprimeltexx) | done using claude.ai

## Galaxy A3 2015 (a3ulte / a3lte, SM-A300F / SM-A300FU) port — `a3ulte` branch

`./build.sh a3ulte RELEASE` builds `boot-a3ulte.img` for the Galaxy A3 2015
(MSM8916, 1 GB RAM, 540x960 Super AMOLED). Boot it from lk2nd the same way
as rossa (`fastboot boot`, or wrap it with `--base 0x80000000
--kernel_offset 0x00080000 --pagesize 2048`).

**Status: builds, not tested on hardware yet.** It is the rossa port with
the A3's hardware taken from mainline Linux (`msm8916-samsung-a3u-eur.dts`,
`msm8916-samsung-a2015-common.dtsi`, msm8916-mainline 6.12.1; the 7.0 WIP
tree has no relevant changes) and lk2nd (`msm8916-samsung.dts`). Everything
not listed below is shared with rossa and expected to behave the same.

| | A3 2015 | Source / notes |
|---|---|---|
| Display | 540x960, 32bpp at `0x8e000000` | S6E88A0-AMS452EF01, **video mode**, so PrePi's pipe re-pointing works as on rossa. The stock bootloader keeps the splash running (lk2nd has no panel node for the A3) |
| Memory map | 1 GB, `DeviceMemoryMapA3ulte.h` | Samsung TZ carve-out `0x85500000+0xB00000` (same as J5), MPSS `0x86800000+0x5400000`, all inside the reserved `0x86700000-0x8D300000` |
| Touchscreen | Zinitix BT541, new `ZinitixTouchDxe` | blsp_i2c5 bit-banged: SDA 18, SCL 19, addr `0x20`, INT 13, vdd_tsp_a enable 73. Power-on sequence, init and report format from Linux `zinitix.c` |
| Keys | Vol+ 107, Home 109, Vol- RESIN, Power KPDPWR | same as rossa |
| Capacitive keys | Recents = Enter, Back = Esc | ABOV MCU with the TC360 protocol (Linux `tm2-touchkey.c`), i2c-gpio SDA 16 / SCL 17, addr `0x20`, power GPIO 86 + LED GPIO 60. `-D NO_TOUCHKEYS` leaves them out |
| Vibration | enable GPIO 76, PWM GPIO 50 | `PlatformPeiLib` holds the PWM input of the haptic driver at 100% so GPIO 76 alone switches the motor (diagnostics, `rossakeys.sys`) |
| Battery / charging | RT5033 | same pins as rossa (charger 22/23 @`0x34`, gauge 14/15 @`0x35`); the driver's 4.35 V / 700 mA limits are below the A3's |
| USB OTG host | RT5033 boost + **SM5502** MUIC | MUIC on i2c-gpio 105/106 @`0x25`, same registers as the SM5504/SM5703 |
| SD card / eMMC | card detect GPIO 38 | same as rossa |
| DSDT | `Dsdt/DsdtA3ulte.asl` → `DSDT_A3ULTE.AML` | touch is `A3TS0001` (no Windows driver yet), keys/motor `RSKY0001` and battery `RSBT0001` keep rossa's IDs and layout so `rossakeys.sys`/`rossabatt.sys` should work; no `BKLT` (the AMOLED panel has no DCS 0x51 brightness) |
| DTB | `ImageResources/a3ulte.dts` | msm-id `0xce`, board-id `0xce08ff01` revisions 0-15 (lk2nd: A300F/FU = 1, A300YZ = 3) |

Shared code changed on this branch (rossa and the J5 targets build and
behave as before):

- `PcdMotorEnableGpio` / `PcdMotorPwmGpio`: RossaDiag buzzes the configured
  motor GPIO (rossa sets 72), `PlatformPeiLib` parks a PWM-driven motor.
- `KeypadDeviceImplLib`: optional TC360 capacitive keys (`PcdTouchkey*`,
  off by default).
- The boot menu entry is now just "Diagnostics".

First things to check on a real A3: the screen comes up (if it stays black,
the bootloader left the splash on a pipe/mixer PrePi doesn't look at),
RossaDiag shows the Zinitix touch and "Cores 4 of 4", and the touch
orientation (`PcdTouchInvertX/Y` if it is mirrored).

## Galaxy Core Prime LTE (rossa, SM-G360F) port

`./build.sh rossa RELEASE` builds `boot-rossa.img` for the Core Prime LTE
(MSM8916, 1 GB RAM, 480x800). Wrap and boot it from lk2nd exactly like the J5
image below (`--base 0x80000000 --kernel_offset 0x00080000 --pagesize 2048`).

Status as of 14 Nov 2026, tested on an SM-G360F unless marked otherwise.
"WinPE" means Windows PE 10 (ADK 2004 / build 19041, ARM64) from the SD card
with the [rossa-winpe](https://github.com/notnoelchannel2/rossa-winpe) drivers.

### Working

| | UEFI | WinPE | Notes |
|---|---|---|---|
| Display (framebuffer) | yes | yes | 480x800, PrePi re-points the MDP pipe at `0x8e000000` |
| Memory map | yes | yes | 1 GB, `Include/Configuration/DeviceMemoryMapRossa.h` |
| SD card (SDC2) | yes | yes | card detect GPIO 38; SDSC (<= 2 GB, byte-addressed) cards work; WinPE uses inbox `sdbus`/`sdstor` |
| eMMC (SDC1) | yes, read-only | hidden | HS200; write-protected on purpose (Android/bootloader live there) |
| Touchscreen (IST3038) | yes | yes | legacy IST30xx protocol; `rossatouch.sys` in WinPE |
| On-screen keyboard | | yes | `rossaosk.exe` is the WinPE shell, docks under the command prompt, buzzes on taps |
| Keys (Vol+/Vol-/Home/Power) | yes | yes | UEFI: Up/Down/Esc/Enter; WinPE (`rossakeys.sys`): Up/Down/Enter/Esc |
| Vibration motor | yes (RossaDiag) | yes | GPIO72; `vibrate.exe <ms>`, key and keyboard buzz |
| CPU cores | 4 | 4 parked | `MpParkDxe`: SCM cold-boot address + ACC power-up, cores parked for the ACPI MP Parking Protocol; MADT lists only cores that arrived. RossaDiag shows "Cores 4 of 4"; WinPE runs on all 4 (`bcd.cmd smp`) |
| Battery / charging | yes | yes | `Rt5033ChargerDxe` / `rossabatt.sys` (ACPI `RSBT0001`): CV 4.35 V, 500 mA input, over-voltage stop; %, voltage and charger state |
| Reset / power off | yes | yes | PMIC PS_HOLD (no PSCI in Samsung's TrustZone); runtime MMIO mapped for the OS, so `wpeutil reboot` works |
| Boot menu, RossaDiag, TouchTest | yes | | RossaDiag: CPU/GIC/PMIC/display/touch/battery/disks/cores/motor |
| WinPE boot from SD | | yes | `make-rossa-sd.cmd`; `\rossape\autorun.cmd` + `diag.txt` on the card |

### Not working yet / untested

| | Status |
|---|---|
| Full Windows 10 | boots (1809 ARM64 from a 16 GB card, all 4 cores) with `make-rossa-win10.cmd`; SD runs at 25 MHz (high speed hidden from the OS). First-run setup still being sorted out; `rossabatt` left out for now (crashed with Windows' Charge Arbitration Driver, fixed in 1.0.1, untested) |
| Screen brightness | written, **untested**: `rossabl.sys` (ACPI `RSBL0001`) sends the panel's DCS brightness command through the DSI command DMA; `brightness.exe 0-100 / up / down` |
| USB OTG host | written, **untested**: `UsbHostDxe` with RT5033 boost + SM5504 MUIC routing, DSDT `_DSM` restores host mode after Windows' EHCI reset. OTG adapter must be plugged in before booting |
| Battery icon in WinPE | needs `battc.sys` in the image; readings work through `battstat` |

### To do

- Finish first-run setup of full Windows 10; test rossabatt 1.0.1 there
- Test USB host (keyboard / USB stick through an OTG adapter), then hot-plug
- Test screen brightness (`rossabl`)
- Not planned: Wi-Fi (WCN3620) and audio (WCD9306/LPASS) need Qualcomm
  firmware loading and remote-processor drivers that don't exist for Windows

What differs from the J5 targets (all selected by `Devices/rossa.dsc`):

- **RAM**: `PcdSystemMemorySize = 0x40000000`. The UEFI stack/HOB region is
  taken from the top of that size, so the J5's 1.5 GB value would put it in
  memory that doesn't exist on rossa. The memory map ends at `0xC0000000` and
  uses rossa's Samsung TZ carve-out (`0x85A00000+0x600000`, mainline
  `msm8916-samsung-fortuna-common.dtsi`) and its larger modem region
  (`0x86800000+0x5800000`), all inside the reserved `0x86700000-0x8D300000`.
- **Display**: stride comes from `PcdMipiFrameBufferWidth` instead of a
  hard-coded 720, the pipe that the bootloader staged is detected, and its
  scanout address is set to `PcdMipiFrameBufferAddress`. Samsung's bootloader
  keeps its splash at `0x83000000`, which is ordinary RAM here.
- **Touch**: rossa's Imagis IST3038 speaks the older IST30xx protocol
  (status and coordinates streamed from register `0x20`, Samsung
  `imagis_core3` / mainline `imagis,ist3038`). `Ist3038cTouchDxe` now probes
  the chip id and picks the protocol; `PcdTouchImagisLegacy` only sets the
  probe order. Pins: SDA 18, SCL 19, INT 13, VDD enable 73, address `0x50`.
- **Keys**: `GenericKeypadDeviceDxe`/`KeypadDxe` are in the rossa FV, with
  Vol+ on GPIO 107, Home on GPIO 109, Vol- on PM8916 RESIN and Power on
  KPDPWR (PCDs `PcdKeypad*`, all off for the J5 targets).
- **DTB**: `ImageResources/rossa.dts`, msm-id `0xce`, board-id
  `0xce08ff01` revisions 0-15.
- **SMBIOS**: vendor/product come from the device PCDs, memory size from
  `PcdSystemMemorySize`.
- **DSDT**: rossa uses `AcpiTables/DSDT_ROSSA.AML` (source
  `Dsdt/DsdtRossa.asl`): the touch device is `RSTS0001` with GPIO 13/18/19/73,
  for the `rossatouch` Windows driver.
- **No keyboard**: the boot manager menu comes up instead of the UEFI Shell
  (`PcdBootToMenu`); the Shell, *Touch Test* and *Diagnostics (RossaDiag)* are
  menu entries. An SD card with `EFI\BOOT\BOOTAA64.EFI` still boots on its own.
- **SPMI**: MSM8916 has a v2 PMIC arbiter (`PcdSpmiVersion = 2`); with the
  old v1 setting the first PMIC access (keys) ended in an SError.
- **Reset**: `Msm8916ResetSystemLib` sets the PMIC PS_HOLD action (hard reset
  or shutdown) and drops PS_HOLD, like Qualcomm's LK.

## Tutorial

The stock Samsung bootloader won't boot it directly; use [lk2nd](https://github.com/msm8916-mainline/lk2nd)
(23.1 tested) as a chain bootloader, then:

```bash
fastboot boot uefi-rossa-androidboot.img
```

or `fastboot flash boot uefi-rossa-androidboot.img` from lk2nd.

## What changed vs. upstream

**Windows boot**
- `SD()`/`eMMC()` device-path nodes so bootmgr can reopen its boot device (was 0xc000000f).
- Single-core MADT in the old 40-byte GICC format 10586 expects. The 4-core table is
  kept as `Prebuilt_8916/Madt_4core_mppark.acpi`. GTDT trimmed to 0x60.
- Memory map: Samsung TrustZone regions reserved (`0x85500000+0xB00000`,
  `0x86680000+0x80000`), overlap with MPSS removed, small `RtData` regions retyped
  so runtime regions stay 64 KB aligned (`ExitBootServices` failed otherwise).
- DSDT: `SDC2` with an embedded `SDMM` child (`_RMV 0`, card detect is a GPIO),
  `SDC1` hidden, `TSC0`/`J5TS0001` touch device (TLMM GPIO pages), `USB0`.

**Storage (`SdhciMMCHSDxe`)**
- eMMC always initialized (8-bit HS200, falls back without HS200), read-only.
- SD card probed only when present (card detect GPIO 38), 4-bit up to 50 MHz.
- SDC2 pin config bit offsets fixed; unaligned reads go through a bounce buffer
  (they hung DEBUG builds and corrupted memory in RELEASE); per-clock-change 8 MB
  framebuffer wipe removed.

**GPIO** — TLMM register stride PCDs fixed (0x1000 per GPIO on MSM8916).

**Touch (`Ist3038cTouchDxe`, `GpioI2cLib`)** — Imagis IST3038C on bit-banged I2C
(SDA 18, SCL 19, INT 13, VDD enable 108), protocol from Linux `imagis.c`, publishes
`EFI_ABSOLUTE_POINTER_PROTOCOL`. Pins and axis flips are PCDs in `Devices/j5xlte.dsc`.

**Apps in the FV** — `TouchTest` (draws touches), `J5Diag` (EL, timer, GIC, PSCI info).

**Build** — `Devices/j5xlte.dsc`/`.fdf`, `ImageResources/j5xlte.dts`/`.dtb`
(board-id `0xce08ff01 0x04`). j5nlte still builds.

## Known gaps
- USB OTG host: `UsbHostDxe` brings up VBUS boost, MUIC routing and the ChipIdea
  controller, but no device is detected yet.
- Only one CPU core; needs PSCI or MP parking in the firmware.
- `DeviceMemoryMap.h` maps 1.5 GB; the top 256 MB of the 2 GB isn't mapped yet.
