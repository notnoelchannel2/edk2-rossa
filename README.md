# EDK2 for the Galaxy J5 2016 (j5xlte, SM-J510) (ts entierly slopped btw dont take too serious (Rest In Peace Rest In Peace Fish #29345823-#29347503 that died for this))

<img width="875" height="713" alt="photo_2026-08-10_19-11-33" src="https://github.com/user-attachments/assets/98ab62c9-6815-4e60-8ef1-d5ecc39fab57" />


## Galaxy Core Prime LTE (rossa, SM-G360F) port

`./build.sh rossa RELEASE` builds `boot-rossa.img` for the Core Prime LTE
(MSM8916, 1 GB RAM, 480x800). Wrap and boot it from lk2nd exactly like the J5
image below (`--base 0x80000000 --kernel_offset 0x00080000 --pagesize 2048`).

Status as of 14 Nov 2026, tested on an SM-G360F unless marked otherwise.
"WinPE" means Windows PE 10 (ADK 2004, ARM64) from the SD card with the
[rossa-winpe](https://github.com/notnoelchannel2/rossa-winpe) drivers.

### Working

| | UEFI | WinPE | Notes |
|---|---|---|---|
| Display (framebuffer) | yes | yes | 480x800, PrePi re-points the MDP pipe at `0x8e000000` |
| Memory map | yes | yes | 1 GB, `Include/Configuration/DeviceMemoryMapRossa.h` |
| SD card (SDC2) | yes | yes | card detect GPIO 38; SDSC (<= 2 GB, byte-addressed) cards work; WinPE uses inbox `sdbus`/`sdstor` |
| eMMC (SDC1) | yes, read-only | hidden | HS200; write-protected on purpose (Android/bootloader live there) |
| Touchscreen (IST3038) | yes | yes | legacy IST30xx protocol; `rossatouch.sys` in WinPE |
| Keys (Vol+/Vol-/Home/Power) | yes | yes | UEFI: Up/Down/Esc/Enter; WinPE (`rossakeys.sys`): Up/Down/Enter/Esc |
| CPU cores | 4 | 4 parked | `MpParkDxe`: SCM cold-boot address + ACC power-up, cores parked for the ACPI MP Parking Protocol; MADT lists only cores that arrived. RossaDiag shows "Cores 4 of 4" |
| Battery / charging | yes | yes | `Rt5033ChargerDxe` / `rossabatt.sys` (ACPI `RSBT0001`): CV 4.35 V, 500 mA input, over-voltage stop; %, voltage and charger state |
| Reset / power off | yes | yes | PMIC PS_HOLD (no PSCI in Samsung's TrustZone) |
| Boot menu, RossaDiag, TouchTest | yes | | RossaDiag: CPU/GIC/PMIC/display/touch/battery/disks/cores/motor |
| WinPE boot from SD | | yes | build 19041, `make-rossa-sd.cmd`; `\rossape\autorun.cmd` + `diag.txt` on the card |

### Not working yet

| | Status |
|---|---|
| On-screen keyboard (`rossaosk.exe`) | Windows never starts it (installed as the WinPE shell, but runs zero instructions). `diag.txt` now records its exit code to find out why |
| Vibration motor | `rossakeys.sys` drives GPIO72 like Samsung's driver, but nothing is felt in WinPE. RossaDiag has a motor test to tell hardware from driver |
| Windows using 4 cores | firmware parks all 4; untested with `bcd.cmd smp` (the card still boots with `onecpu`) |
| Battery icon in WinPE | needs `battc.sys` in the image; readings work through `battstat` |
| USB OTG host | written, **untested**: `UsbHostDxe` with RT5033 boost + SM5504 MUIC routing, DSDT `_DSM` restores host mode after Windows' EHCI reset. Plug the OTG adapter in before booting |

### To do

- Fix rossaosk start-up (waiting on `diag.txt` exit code)
- Find out why the motor doesn't run (RossaDiag motor test)
- Test USB host (keyboard / USB stick through an OTG adapter)
- Test Windows on all 4 cores
- Hot-plug for USB OTG (now only detected at boot)
- Screen brightness (backlight)
- Full Windows 10 from a larger SD card (slow with 1 GB RAM)
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

## Status

| | UEFI | Windows PE |
|---|---|---|
| Display (framebuffer) | yes | yes |
| SD card (SDC2) | yes | yes (inbox `sdbus`/`sdstor`) |
| eMMC (SDC1) | read-only | hidden on purpose |
| Touchscreen (IST3038C) | yes | yes ([j5touch](https://github.com/jimkoutso2008/j5touch)) |
| CPU cores | 1 | 1 (no PSCI / MP parking yet) |
| USB OTG host | no (WIP, `UsbHostDxe`) | no |

**The eMMC is protected:** the firmware reports it read-only and refuses all writes
(`EFI_WRITE_PROTECTED`), and the DSDT hides it from Windows (`SDC1._STA = 0`).
It holds the phone's bootloader and Android partitions.

## Building (WSL Debian)

```bash
sudo apt install build-essential uuid-dev iasl nasm python3 python-is-python3 gcc-aarch64-linux-gnu device-tree-compiler mkbootimg git
```

```bash
git clone https://github.com/jimkoutso2008/edk2-msm8916-j5xlte && cd edk2-msm8916-j5xlte
```

```bash
git submodule update --init --depth 1 edk2 edk2-platforms && git -C edk2 submodule update --init --depth 1
```

```bash
mkdir -p workspace && (set --; . edk2/edksetup.sh && make -C edk2/BaseTools -j"$(nproc)")
```

```bash
./build.sh j5xlte RELEASE
```

`build.sh [device] [DEBUG|RELEASE]` writes `boot-<device>.img` (gzip'd FD + DTB).
Extra `build` flags go in `$BUILD_EXTRA` (e.g. `BUILD_EXTRA="-D USB_SELFTEST"`).
Clone into the WSL filesystem (`~/...`), not `/mnt/c`.

Wrap it as an Android boot image for lk2nd:

```bash
mkbootimg --kernel boot-j5xlte.img --base 0x80000000 --kernel_offset 0x00080000 --pagesize 2048 --cmdline "" -o uefi-j5xlte-androidboot.img
```

## Booting

The stock Samsung bootloader won't boot it directly; use [lk2nd](https://github.com/msm8916-mainline/lk2nd)
(23.1 tested) as a chain bootloader, then:

```bash
fastboot boot uefi-j5xlte-androidboot.img
```

or `fastboot flash boot uefi-j5xlte-androidboot.img` from lk2nd.

### Windows PE from the SD card
1. Copy the WinPE files (`efi\`, `boot\`, `sources\boot.wim`) to a FAT32 SD card.
2. The BCD needs `testsigning on`, `nointegritychecks on` and `onecpu on`.
   [`tools/bcd.cmd`](tools/bcd.cmd) shows, edits or recreates it (elevated cmd).
3. Install [j5touch](https://github.com/jimkoutso2008/j5touch) and
   [j5osk](https://github.com/jimkoutso2008/j5osk) into `boot.wim` for touch input.

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
