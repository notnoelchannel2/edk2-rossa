/*
 * RossaDiag: what the firmware sees on the Galaxy Core Prime LTE (rossa).
 *
 * Read-only and safe to run from the boot manager menu: it only reads
 * registers and protocols, then waits for keys so the output stays on
 * screen (J5Diag returned immediately, which looked like a crash when
 * started from the menu). The J5Diag probes that could hang or abort the
 * phone (PSCI SMC, timer MMIO frame, USB controller reset) are gone.
 *
 * Page 1: CPU, timer, GIC, PMIC, display, touch, storage
 * Page 2: live key test (Vol+/Vol-/Power/Home), Home twice to exit
 */

#include <Uefi.h>
#include <Library/LKEnvLib.h>
#include <Library/ArmGenericTimerCounterLib.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/GpioI2cLib.h>
#include <Library/QcomGpioTlmmLib.h>
#include <Library/TimerLib.h>

#include <Protocol/AbsolutePointer.h>
#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/QcomPm8x41.h>
#include <Protocol/Rt5033Battery.h>

/* Same app on every phone of the family, named after the one it was built for */
#ifdef MSM8916_DEVICE_GPRIMELTEXX
#define DIAG_NAME L"GprimeDiag"
#else
#define DIAG_NAME L"RossaDiag"
#endif

#define GICD_TYPER      0x004
#define GICD_IIDR       0x008
#define GICC_IIDR       0x0FC
#define SPMI_HW_VERSION 0xF000   /* PcdSpmiBaseAddress + this */

STATIC UINT64
ReadCurrentEL (
  VOID
  )
{
  UINT64 Value;

  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (Value));
  return (Value >> 2) & 3;
}

STATIC VOID
ShowCpu (
  VOID
  )
{
  UINT64 Freq;
  UINT64 Count0;
  UINT64 Count1;
  UINTN  GicD = (UINTN)PcdGet64 (PcdGicDistributorBase);
  UINTN  GicC = (UINTN)PcdGet64 (PcdGicInterruptInterfaceBase);
  UINT32 Typer;

  Print (L"CPU   EL%lu  MIDR %08lx  MPIDR %08lx\n",
         ReadCurrentEL (), (UINT64)ArmReadMidr (), (UINT64)ArmReadMpidr ());

  Freq   = ArmGenericTimerGetTimerFreq ();
  Count0 = ArmGenericTimerGetSystemCount ();
  gBS->Stall (100000);
  Count1 = ArmGenericTimerGetSystemCount ();
  Print (L"Timer %lu Hz, 100ms = %lu ticks\n", Freq, Count1 - Count0);

  Typer = MmioRead32 (GicD + GICD_TYPER);
  Print (L"GIC   %u IRQs, GICD_IIDR %08x GICC_IIDR %08x\n",
         ((Typer & 0x1F) + 1) * 32, MmioRead32 (GicD + GICD_IIDR), MmioRead32 (GicC + GICC_IIDR));
  Print (L"RAM   %lu MB\n", PcdGet64 (PcdSystemMemorySize) >> 20);

  /* MpParkDxe leaves the mask of parked cores at 0x80305000 */
  {
    UINT32 Mask = MmioRead32 (0x80305000);
    UINTN  N    = 0;
    UINTN  I;
    for (I = 0; I < 4; I++) {
      N += (Mask >> I) & 1;
    }
    Print (L"Cores %u of 4 online (mask 0x%x)%s\n", N, Mask,
           N == 4 ? L"" : L" - others not started, see the firmware log");
  }
}

STATIC VOID
ShowPmic (
  VOID
  )
{
  QCOM_PM8X41_PROTOCOL *Pmic;
  UINT32               ArbVer;

  ArbVer = MmioRead32 ((UINTN)PcdGet64 (PcdSpmiBaseAddress) + SPMI_HW_VERSION);
  Print (L"SPMI  arbiter version %08x (%a)\n", ArbVer, ArbVer >= 0x20010000 ? "v2" : "v1");

  if (EFI_ERROR (gBS->LocateProtocol (&gQcomPm8x41ProtocolGuid, NULL, (VOID **)&Pmic))) {
    Print (L"PMIC  driver not loaded\n");
    return;
  }

  Print (L"PMIC  rev %u, PON reason 0x%02x, POFF reason 0x%02x\n",
         Pmic->pm8x41_get_pmic_rev (), Pmic->pm8x41_get_pon_reason (),
         Pmic->pm8x41_get_pon_poff_reason1 ());
}

STATIC VOID
ShowDisplay (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;

  if (EFI_ERROR (gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop)) &&
      EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop))) {
    Print (L"GOP   none\n");
    return;
  }

  Print (L"GOP   %ux%u, FB 0x%lx (%lu KB)\n",
         Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution,
         (UINT64)Gop->Mode->FrameBufferBase, (UINT64)Gop->Mode->FrameBufferSize >> 10);
}

/*
 * Bit-banged I2C register access for the probes below. The touch driver
 * polls its chip from a TPL_NOTIFY timer on the same pins, so keep it out
 * while we talk.
 */
STATIC EFI_STATUS
DiagI2cRead (
  IN  GPIO_I2C_BUS  *Bus,
  IN  UINT8         Address,
  IN  CONST UINT8   *Reg,
  IN  UINTN         RegLen,
  IN  BOOLEAN       SplitTransfer,
  OUT UINT8         *Data,
  IN  UINTN         Len
  )
{
  EFI_STATUS Status;
  EFI_TPL    OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (SplitTransfer) {
    Status = GpioI2cTransfer (Bus, Address, Reg, RegLen, NULL, 0);
    if (!EFI_ERROR (Status)) {
      MicroSecondDelay (50);
      Status = GpioI2cTransfer (Bus, Address, NULL, 0, Data, Len);
    }
  } else {
    Status = GpioI2cTransfer (Bus, Address, Reg, RegLen, Data, Len);
  }
  gBS->RestoreTPL (OldTpl);

  return Status;
}

STATIC VOID
ShowTouch (
  VOID
  )
{
  EFI_ABSOLUTE_POINTER_PROTOCOL *Touch = NULL;
  EFI_ABSOLUTE_POINTER_PROTOCOL *This;
  EFI_HANDLE                    *Handles;
  UINTN                         Count;
  UINTN                         Index;

  /*
   * The console splitter publishes a virtual pointer (65536x65536) on the
   * ConIn handle even when no touch driver loaded: skip it and look for the
   * real device.
   */
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiAbsolutePointerProtocolGuid, NULL, &Count, &Handles))) {
    for (Index = 0; Index < Count; Index++) {
      if (Handles[Index] == gST->ConsoleInHandle) {
        continue;
      }
      if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiAbsolutePointerProtocolGuid, (VOID **)&This))) {
        Touch = This;
        break;
      }
    }
    FreePool (Handles);
  }

  if (Touch == NULL) {
    Print (L"Touch DRIVER NOT LOADED (chip did not answer at boot)\n");
  } else {
    Print (L"Touch driver ok, %lux%lu\n", Touch->Mode->AbsoluteMaxX, Touch->Mode->AbsoluteMaxY);
  }

  /* Zinitix (BT541 & co, address 0x20): ask the chip itself */
  if (FixedPcdGet8 (PcdTouchI2cAddress) == 0x20 && FixedPcdGet32 (PcdTouchI2cSdaGpio) != 0xFFFFFFFF) {
    GPIO_I2C_BUS Bus;
    UINT8        Reg[2];
    UINT8        Rev[2];
    UINT8        Fw[2];
    EFI_STATUS   Status;

    Bus.SdaGpio      = FixedPcdGet32 (PcdTouchI2cSdaGpio);
    Bus.SclGpio      = FixedPcdGet32 (PcdTouchI2cSclGpio);
    Bus.HalfPeriodUs = 5;

    Reg[0] = 0x11;  /* ZINITIX_CHIP_REVISION */
    Reg[1] = 0x00;
    Status = DiagI2cRead (&Bus, 0x20, Reg, 2, TRUE, Rev, 2);
    Reg[0] = 0x12;  /* ZINITIX_FIRMWARE_VERSION */
    if (!EFI_ERROR (Status)) {
      Status = DiagI2cRead (&Bus, 0x20, Reg, 2, TRUE, Fw, 2);
    }
    if (EFI_ERROR (Status)) {
      Print (L"      Zinitix @0x20 on i2c %u/%u: no answer (%r), INT gpio%u=%u\n",
             Bus.SdaGpio, Bus.SclGpio, Status, FixedPcdGet32 (PcdTouchIntGpio),
             (UINT32)gGpioTlmm->Get (FixedPcdGet32 (PcdTouchIntGpio)));
    } else {
      Print (L"      Zinitix chip rev %02x%02x, firmware %02x%02x, INT gpio%u=%u\n",
             Rev[1], Rev[0], Fw[1], Fw[0], FixedPcdGet32 (PcdTouchIntGpio),
             (UINT32)gGpioTlmm->Get (FixedPcdGet32 (PcdTouchIntGpio)));
    }
  }
}

/*
 * Motion / proximity sensors of the Grand Prime (fortuna) on the blsp_i2c2
 * pins, bit-banged: GPIO6 SDA, GPIO7 SCL (mainline
 * msm8916-samsung-fortuna-common.dtsi + grandprimelte.dts, Samsung
 * msm8916-sec-fortuna-eur-r02.dtsi).
 *   0x10 Bosch BMC150 accelerometer   (INT gpio115)   bmc150-accel-core.c
 *   0x12 Bosch BMC150 magnetometer                    bmc150_magn.c
 *   0x44 Sharp GP2A proximity         (INT gpio113, enable gpio8)
 * Supplies: PM8916 L17 (2.85 V) and L5 (1.8 V IO).
 * UEFI has no use for them; this only proves they are powered and reachable
 * for an OS driver.
 */
#ifdef MSM8916_DEVICE_GPRIMELTEXX
#define SENSOR_I2C_SDA          6
#define SENSOR_I2C_SCL          7
#define PM8916_L17_BASE         0x15000
#define PM8916_LDO_STATUS1      0x08    /* bit 7: VREG_OK */
#define PM8916_LDO_EN_CTL       0x46    /* bit 7: enabled */

#define BMC150_ACCEL_REG_CHIP_ID   0x00   /* 0xFA: BMC150/BMA255, 0xF9: BMA250E, 0xFB: BMA280 */
#define BMC150_ACCEL_REG_XOUT_L    0x02   /* X, Y, Z: 12 bit, left justified, little endian */
#define BMC150_MAGN_REG_CHIP_ID    0x40   /* 0x32, readable once powered */
#define BMC150_MAGN_REG_POWER      0x4B   /* bit 0: power control */

STATIC INT32
Bmc150Axis (
  IN UINT8  Lsb,
  IN UINT8  Msb
  )
{
  /* 12 bit signed, +-2 g range after reset: 1024 LSB per g */
  INT32 Raw = (INT16)((Msb << 8) | Lsb) >> 4;

  return (Raw * 1000) / 1024;
}

STATIC VOID
ShowSensors (
  VOID
  )
{
  QCOM_PM8X41_PROTOCOL *Pmic;
  GPIO_I2C_BUS         Bus;
  EFI_STATUS           Status;
  UINT8                Reg;
  UINT8                Data[6];
  UINT8                Cmd[2];

  if (!EFI_ERROR (gBS->LocateProtocol (&gQcomPm8x41ProtocolGuid, NULL, (VOID **)&Pmic))) {
    Print (L"Sensor supply L17: en %02x, status %02x\n",
           Pmic->pm8x41_reg_read (PM8916_L17_BASE + PM8916_LDO_EN_CTL),
           Pmic->pm8x41_reg_read (PM8916_L17_BASE + PM8916_LDO_STATUS1));
  }

  Bus.SdaGpio      = SENSOR_I2C_SDA;
  Bus.SclGpio      = SENSOR_I2C_SCL;
  Bus.HalfPeriodUs = 5;
  GpioI2cInit (&Bus);

  Reg    = BMC150_ACCEL_REG_CHIP_ID;
  Status = DiagI2cRead (&Bus, 0x10, &Reg, 1, FALSE, Data, 1);
  if (EFI_ERROR (Status)) {
    Print (L"Accel  BMC150 @0x10: no answer (%r)\n", Status);
  } else {
    UINT8 Id = Data[0];

    Reg    = BMC150_ACCEL_REG_XOUT_L;
    Status = DiagI2cRead (&Bus, 0x10, &Reg, 1, FALSE, Data, 6);
    if (EFI_ERROR (Status)) {
      Print (L"Accel  id %02x, data read failed (%r)\n", Id, Status);
    } else {
      Print (L"Accel  id %02x%a, x %d y %d z %d mg\n", Id, Id == 0xFA ? " (BMC150)" : "",
             Bmc150Axis (Data[0], Data[1]), Bmc150Axis (Data[2], Data[3]), Bmc150Axis (Data[4], Data[5]));
    }
  }

  /* The magnetometer only shows its id once out of suspend; put it back after */
  Cmd[0] = BMC150_MAGN_REG_POWER;
  Cmd[1] = 0x01;
  Status = GpioI2cTransfer (&Bus, 0x12, Cmd, 2, NULL, 0);
  if (EFI_ERROR (Status)) {
    Print (L"Magn   BMC150 @0x12: no answer (%r)\n", Status);
  } else {
    gBS->Stall (5000);
    Reg    = BMC150_MAGN_REG_CHIP_ID;
    Status = DiagI2cRead (&Bus, 0x12, &Reg, 1, FALSE, Data, 1);
    Print (L"Magn   id %02x%a\n", EFI_ERROR (Status) ? 0 : Data[0],
           (!EFI_ERROR (Status) && Data[0] == 0x32) ? " (BMC150)" : "");
    Cmd[1] = 0x00;
    GpioI2cTransfer (&Bus, 0x12, Cmd, 2, NULL, 0);
  }

  Reg    = 0x00;
  Status = DiagI2cRead (&Bus, 0x44, &Reg, 1, FALSE, Data, 1);
  if (EFI_ERROR (Status)) {
    Print (L"Prox   GP2A @0x44: no answer (%r)\n", Status);
  } else {
    Print (L"Prox   GP2A @0x44 answers (reg0 %02x)\n", Data[0]);
  }
}
#endif

STATIC VOID
ShowStorage (
  VOID
  )
{
  EFI_HANDLE            *Handles;
  UINTN                 Count;
  UINTN                 Index;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  UINTN                 Shown = 0;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles))) {
    Print (L"Disks none\n");
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo)) ||
        BlockIo->Media->LogicalPartition) {
      continue;
    }

    Print (L"Disk  %a: %lu MB%a\n",
           BlockIo->Media->RemovableMedia ? "SD card" : "eMMC   ",
           MultU64x32 (BlockIo->Media->LastBlock + 1, BlockIo->Media->BlockSize) >> 20,
           BlockIo->Media->ReadOnly ? ", read-only" : "");
    Shown++;
  }

  if (Shown == 0) {
    Print (L"Disks none\n");
  }
  if (Shown == 1) {
    Print (L"      (no SD card detected)\n");
  }

  FreePool (Handles);
}

STATIC CONST CHAR16 *
KeyName (
  IN EFI_INPUT_KEY  *Key
  )
{
  switch (Key->ScanCode) {
    case SCAN_UP:   return L"Vol+ (Up)";
    case SCAN_DOWN: return L"Vol- (Down)";
    case SCAN_ESC:  return L"Home (Esc)";
    default:        break;
  }

  if (Key->UnicodeChar == CHAR_CARRIAGE_RETURN) {
    return L"Power (Enter)";
  }

  return L"other key";
}

STATIC VOID Buzz (IN UINTN Ms);

STATIC VOID
KeyTest (
  VOID
  )
{
  EFI_INPUT_KEY Key;
  UINTN         Index;
  BOOLEAN       HomeOnce = FALSE;

  Print (L"\nKey test: press keys (each one buzzes). Home twice = exit.\n");
  gST->ConIn->Reset (gST->ConIn, FALSE);

  for ( ; ; ) {
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
    if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      continue;
    }

    Print (L"  %s\n", KeyName (&Key));
    Buzz (80);

    if (Key.ScanCode == SCAN_ESC) {
      if (HomeOnce) {
        return;
      }
      HomeOnce = TRUE;
    } else {
      HomeOnce = FALSE;
    }
  }
}

/* Vibration motor: GPIO72 high = on (Samsung msm_vibrator.c, mainline regulator-haptic) */
#define MOTOR_TLMM_CFG     0x01048000
#define MOTOR_TLMM_IN_OUT  0x01048004

STATIC VOID
Buzz (
  IN UINTN  Ms
  )
{
  UINT32 Cfg = MmioRead32 (MOTOR_TLMM_CFG);

  /* GPIO function, no pull, output enable; keep drive strength */
  MmioWrite32 (MOTOR_TLMM_CFG, (Cfg & ~0x3Fu) | BIT9);
  MmioWrite32 (MOTOR_TLMM_IN_OUT, BIT1);
  gBS->Stall (Ms * 1000);
  MmioWrite32 (MOTOR_TLMM_IN_OUT, 0);
}

/* What the OS will see on the SD slot (SDC2 SDHCI caps at 0x07864940/44) */
STATIC VOID
ShowSdCaps (
  VOID
  )
{
  UINT32 C0   = MmioRead32 (0x07864940);
  UINT32 C1   = MmioRead32 (0x07864944);
  UINT8  Hc1  = MmioRead8 (0x07864928);
  UINT16 Clk  = MmioRead16 (0x0786492C);
  UINT16 Hc2  = MmioRead16 (0x0786493E);
  UINT32 Div  = ((Clk >> 8) & 0xFF) | (((Clk >> 6) & 0x3) << 8);
  UINT32 Base = (C0 >> 8) & 0xFF;
  UINT32 Khz  = (Div == 0) ? Base * 1000 : (Base * 1000) / (2 * Div);

  Print (L"SD caps %08x/%08x: HS %s (must be off), 1.8V %s, UHS %s\n", C0, C1,
         (C0 & BIT21) ? L"ON" : L"off", (C0 & BIT26) ? L"ON" : L"off",
         (C1 & 0x7) ? L"on" : L"off");
  Print (L"SD caps override %08x/%08x\n", MmioRead32 (0x07864A1C), MmioRead32 (0x07864A20));
  Print (L"SD firmware bus: %u kHz, %u-bit, HS %s, UHS mode %u\n", Khz,
         (Hc1 & BIT1) ? 4 : 1, (Hc1 & BIT2) ? L"on" : L"off", Hc2 & 0x7);
}

STATIC VOID
ShowMotor (
  VOID
  )
{
  Print (L"Motor GPIO72 cfg %08x, buzzing 500 ms...", MmioRead32 (MOTOR_TLMM_CFG));
  Buzz (500);
  Print (L" done (cfg now %08x)\n", MmioRead32 (MOTOR_TLMM_CFG));
}

STATIC VOID
ShowBattery (
  VOID
  )
{
  STATIC CONST CHAR16      *States[] = { L"discharging", L"full", L"charging", L"not charging" };
  RT5033_BATTERY_PROTOCOL  *Batt;
  RT5033_BATTERY_STATUS    St;

  if (EFI_ERROR (gBS->LocateProtocol (&gRt5033BatteryProtocolGuid, NULL, (VOID **)&Batt))) {
    Print (L"Battery: RT5033 driver not loaded (no answer from charger/gauge?)\n");
    return;
  }
  Batt->GetStatus (Batt, &St);
  if (St.GaugeFound) {
    Print (L"Battery: %u%%, %u mV (avg %u, ocv %u)%s\n", St.Capacity, St.VoltageMv, St.AvgVoltageMv,
           St.OcvMv, St.BatteryPresent ? L"" : L", not detected");
  } else {
    Print (L"Battery: fuel gauge not answering (i2c 14/15 @0x35)\n");
  }
  if (St.ChargerFound) {
    Print (L"Charger: %s%s%s, id 0x%02x, regs %02x %02x %02x %02x %02x %02x %02x\n",
           States[St.State & 3], St.State == Rt5033ChgCharging ? (St.FastCharge ? L" (fast)" : L" (pre)") : L"",
           St.SafetyStop ? L", STOPPED: over-voltage" : L"", St.ChargerId,
           St.Regs[0], St.Regs[1], St.Regs[2], St.Regs[3], St.Regs[4], St.Regs[5], St.Regs[6]);
  } else {
    Print (L"Charger: not answering (i2c 22/23 @0x34)\n");
  }
}

EFI_STATUS
EFIAPI
RossaDiagMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  gST->ConOut->ClearScreen (gST->ConOut);
  Print (DIAG_NAME L" - %a\n\n", (CHAR8 *)PcdGetPtr (PcdDeviceProduct));

  ShowCpu ();
  ShowPmic ();
  ShowDisplay ();
  ShowTouch ();
#ifdef MSM8916_DEVICE_GPRIMELTEXX
  ShowSensors ();
#endif
  ShowBattery ();
  ShowSdCaps ();
  ShowMotor ();
  ShowStorage ();

  KeyTest ();
  return EFI_SUCCESS;
}
