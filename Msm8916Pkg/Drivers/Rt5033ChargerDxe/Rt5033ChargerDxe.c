/*
 * Rt5033ChargerDxe - battery charger and fuel gauge for rossa (SM-G360F).
 *
 * Richtek RT5033 on two bit-banged I2C buses (from the Samsung rossa DT):
 *   charger     i2c-gpio SDA 22 / SCL 23, address 0x34
 *   fuel gauge  i2c-gpio SDA 14 / SCL 15, address 0x35
 *
 * Without a driver the charger keeps whatever the bootloader left, which
 * on some boots is nothing useful. This driver sets conservative limits
 * (taken from the Samsung and mainline rossa DTs, then made more cautious)
 * and watches the battery while the firmware runs:
 *
 *   charge voltage (CV)   4.35 V   (Samsung/mainline: 4.40 V)
 *   input current limit   500 mA   (safe on any USB port; no cable detection)
 *   fast charge current   700 mA   (mainline rossa value; capped by input)
 *   pre-charge            450 mA below 3.5 V
 *   end of charge         150 mA, termination enabled
 *   safety timer          6 h      (Samsung charging_total_time)
 *
 * If the battery ever reads above 4.42 V the charger is put in Hi-Z mode
 * (input off) until it falls below 4.25 V. Settings stay in place after
 * ExitBootServices, so Windows/Linux inherit a safe configuration.
 *
 * Register layout from Linux drivers/power/supply/rt5033_{charger,battery}.c.
 */
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/GpioI2cLib.h>
#include <Protocol/Rt5033Battery.h>

#define CHG_ADDR            0x34
#define FG_ADDR             0x35

#define CHG_STAT            0x00
#define CHG_CTRL1           0x01
#define CHG_CTRL2           0x02
#define CHG_DEVICE_ID       0x03
#define CHG_CTRL3           0x04
#define CHG_CTRL4           0x05
#define CHG_CTRL5           0x06

#define STAT_MASK           0x30
#define STAT_TYPE_MASK      0x60
#define STAT_TYPE_FAST      0x60

#define CTRL1_IAICR_MASK    0xE0
#define CTRL1_AICR_500      0x40
#define CTRL1_TE_EN         0x08
#define CTRL1_HZ            0x02
#define CTRL1_MODE_BOOST    0x01

#define CTRL2_CV_MASK       0xFC
#define CTRL2_CV_SHIFT      2
#define CV_MV               4350
#define CV_REG              ((CV_MV - 3650) / 25)

#define CTRL3_TIMER_MASK    0x38
#define CTRL3_TIMER_6H      0x08
#define CTRL3_TIMER_EN      0x01

#define CTRL4_IPREC_MASK    0x18
#define CTRL4_IPREC_450     (1 << 3)
#define CTRL4_EOC_MASK      0x07
#define CTRL4_EOC_150       0x01

#define CTRL5_ICHG_MASK     0xF0
#define CTRL5_ICHG_700      0x00
#define CTRL5_VPREC_MASK    0x0F
#define CTRL5_VPREC_3500    ((3500 - 2300) / 100)

#define FG_OCV_H            0x00
#define FG_VBAT_H           0x02
#define FG_SOC_H            0x04
#define FG_DEVICE_ID        0x09
#define FG_AVG_VOLT_H       0x0A
#define FG_CONFIG_L         0x0D
#define FG_BAT_PRESENT      0x02

#define SAFETY_STOP_MV      4420
#define SAFETY_RESUME_MV    4250
#define MONITOR_PERIOD      (10 * 1000 * 1000 * 10ULL) /* 10 s in 100 ns */

STATIC GPIO_I2C_BUS mChgBus = { 22, 23, 5 };
STATIC GPIO_I2C_BUS mFgBus  = { 14, 15, 5 };

STATIC BOOLEAN   mChargerFound;
STATIC BOOLEAN   mGaugeFound;
STATIC BOOLEAN   mSafetyStop;
STATIC BOOLEAN   mBootServicesDone;
STATIC UINT8     mChargerId;
STATIC EFI_EVENT mMonitorEvent;
STATIC EFI_EVENT mExitBsEvent;

STATIC
EFI_STATUS
RegRead (
  IN  GPIO_I2C_BUS  *Bus,
  IN  UINT8         Addr,
  IN  UINT8         Reg,
  OUT UINT8         *Val
  )
{
  EFI_STATUS Status;
  UINTN      Try;

  for (Try = 0; Try < 3; Try++) {
    Status = GpioI2cTransfer(Bus, Addr, &Reg, 1, Val, 1);
    if (!EFI_ERROR(Status)) {
      return Status;
    }
    MicroSecondDelay(200);
  }
  return Status;
}

STATIC
EFI_STATUS
RegWrite (
  IN GPIO_I2C_BUS  *Bus,
  IN UINT8         Addr,
  IN UINT8         Reg,
  IN UINT8         Val
  )
{
  EFI_STATUS Status;
  UINT8      Buf[2];
  UINTN      Try;

  Buf[0] = Reg;
  Buf[1] = Val;
  for (Try = 0; Try < 3; Try++) {
    Status = GpioI2cTransfer(Bus, Addr, Buf, 2, NULL, 0);
    if (!EFI_ERROR(Status)) {
      return Status;
    }
    MicroSecondDelay(200);
  }
  return Status;
}

/* Read-modify-write, verified by reading back */
STATIC
EFI_STATUS
ChgUpdate (
  IN UINT8  Reg,
  IN UINT8  Mask,
  IN UINT8  Bits
  )
{
  EFI_STATUS Status;
  UINT8      Old;
  UINT8      New;
  UINT8      Check;

  Status = RegRead(&mChgBus, CHG_ADDR, Reg, &Old);
  if (EFI_ERROR(Status)) {
    return Status;
  }
  New = (Old & ~Mask) | (Bits & Mask);
  if (New == Old) {
    return EFI_SUCCESS;
  }
  Status = RegWrite(&mChgBus, CHG_ADDR, Reg, New);
  if (EFI_ERROR(Status)) {
    return Status;
  }
  Status = RegRead(&mChgBus, CHG_ADDR, Reg, &Check);
  if (EFI_ERROR(Status) || ((Check ^ New) & Mask) != 0) {
    DEBUG((EFI_D_WARN, "RT5033: reg 0x%02x wrote 0x%02x read 0x%02x\n", Reg, New, Check));
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

/* 12-bit voltage in two registers, 1.25 mV per step */
STATIC
UINT32
FgReadMv (
  IN UINT8  RegH
  )
{
  UINT8 H;
  UINT8 L;

  if (EFI_ERROR(RegRead(&mFgBus, FG_ADDR, RegH, &H)) ||
      EFI_ERROR(RegRead(&mFgBus, FG_ADDR, RegH + 1, &L))) {
    return 0;
  }
  return ((((UINT32)H << 4) | (L >> 4)) * 1250) / 1000;
}

STATIC
VOID
ConfigureCharger (
  VOID
  )
{
  EFI_STATUS Status = EFI_SUCCESS;

  /* Charger mode (not OTG boost), input limit, termination; leave HZ alone here */
  Status |= ChgUpdate(CHG_CTRL1, CTRL1_IAICR_MASK | CTRL1_TE_EN | CTRL1_MODE_BOOST,
                      CTRL1_AICR_500 | CTRL1_TE_EN);
  Status |= ChgUpdate(CHG_CTRL2, CTRL2_CV_MASK, CV_REG << CTRL2_CV_SHIFT);
  Status |= ChgUpdate(CHG_CTRL3, CTRL3_TIMER_MASK | CTRL3_TIMER_EN, CTRL3_TIMER_6H | CTRL3_TIMER_EN);
  Status |= ChgUpdate(CHG_CTRL4, CTRL4_IPREC_MASK | CTRL4_EOC_MASK, CTRL4_IPREC_450 | CTRL4_EOC_150);
  Status |= ChgUpdate(CHG_CTRL5, CTRL5_ICHG_MASK | CTRL5_VPREC_MASK, CTRL5_ICHG_700 | CTRL5_VPREC_3500);

  DEBUG((EFI_D_INFO, "RT5033: charger %a (CV %u mV, AICR 500 mA, ICHG 700 mA)\n",
         EFI_ERROR(Status) ? "partly configured" : "configured", CV_MV));
}

STATIC
VOID
ReadStatus (
  OUT RT5033_BATTERY_STATUS  *St
  )
{
  UINT8 Val;
  UINTN I;

  ZeroMem(St, sizeof(*St));
  St->ChargerFound = mChargerFound;
  St->GaugeFound   = mGaugeFound;
  St->SafetyStop   = mSafetyStop;
  St->ChargerId    = mChargerId;

  if (mChargerFound) {
    for (I = 0; I < ARRAY_SIZE(St->Regs); I++) {
      RegRead(&mChgBus, CHG_ADDR, (UINT8)I, &St->Regs[I]);
    }
    St->State             = (RT5033_CHG_STATE)((St->Regs[CHG_STAT] & STAT_MASK) >> 4);
    St->PowerSourceOnline = (St->State != Rt5033ChgDischarging);
    St->FastCharge        = (St->Regs[CHG_STAT] & STAT_TYPE_MASK) == STAT_TYPE_FAST;
  }

  if (mGaugeFound) {
    if (!EFI_ERROR(RegRead(&mFgBus, FG_ADDR, FG_CONFIG_L, &Val))) {
      St->BatteryPresent = (Val & FG_BAT_PRESENT) != 0;
    }
    if (!EFI_ERROR(RegRead(&mFgBus, FG_ADDR, FG_SOC_H, &Val))) {
      St->Capacity = Val > 100 ? 100 : Val;
    }
    St->VoltageMv    = FgReadMv(FG_VBAT_H);
    St->AvgVoltageMv = FgReadMv(FG_AVG_VOLT_H);
    St->OcvMv        = FgReadMv(FG_OCV_H);
  }
}

/* Over-voltage guard: Hi-Z cuts the charger input, the phone runs on battery */
STATIC
VOID
EFIAPI
MonitorTick (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32 Mv;

  if (mBootServicesDone || !mChargerFound || !mGaugeFound) {
    return;
  }

  Mv = FgReadMv(FG_VBAT_H);
  if (Mv == 0) {
    return;
  }

  if (!mSafetyStop && Mv >= SAFETY_STOP_MV) {
    if (!EFI_ERROR(ChgUpdate(CHG_CTRL1, CTRL1_HZ, CTRL1_HZ))) {
      mSafetyStop = TRUE;
      DEBUG((EFI_D_WARN, "RT5033: battery at %u mV, charging stopped\n", Mv));
    }
  } else if (mSafetyStop && Mv <= SAFETY_RESUME_MV) {
    if (!EFI_ERROR(ChgUpdate(CHG_CTRL1, CTRL1_HZ, 0))) {
      mSafetyStop = FALSE;
      DEBUG((EFI_D_INFO, "RT5033: battery at %u mV, charging resumed\n", Mv));
    }
  }
}

STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  mBootServicesDone = TRUE;
  /* The OS can't see our Hi-Z flag: never leave the input cut off */
  if (mSafetyStop) {
    ChgUpdate(CHG_CTRL1, CTRL1_HZ, 0);
  }
}

STATIC
EFI_STATUS
EFIAPI
GetStatus (
  IN  RT5033_BATTERY_PROTOCOL  *This,
  OUT RT5033_BATTERY_STATUS    *Status
  )
{
  EFI_TPL OldTpl;

  if (Status == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  /* Keep the monitor timer from interleaving with our bus traffic */
  OldTpl = gBS->RaiseTPL(TPL_CALLBACK);
  ReadStatus(Status);
  gBS->RestoreTPL(OldTpl);
  return (mChargerFound || mGaugeFound) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

STATIC RT5033_BATTERY_PROTOCOL mProtocol = { 1, GetStatus };

EFI_STATUS
EFIAPI
Rt5033ChargerDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE            Handle = NULL;
  RT5033_BATTERY_STATUS St;
  UINT8                 Val;
  EFI_STATUS            Status;

  GpioI2cInit(&mChgBus);
  GpioI2cInit(&mFgBus);

  mChargerFound = !EFI_ERROR(RegRead(&mChgBus, CHG_ADDR, CHG_DEVICE_ID, &mChargerId));
  mGaugeFound   = !EFI_ERROR(RegRead(&mFgBus, FG_ADDR, FG_DEVICE_ID, &Val));
  DEBUG((EFI_D_INFO, "RT5033: charger %a (id 0x%02x), fuel gauge %a\n",
         mChargerFound ? "found" : "missing", mChargerId, mGaugeFound ? "found" : "missing"));

  if (!mChargerFound && !mGaugeFound) {
    return EFI_NOT_FOUND;
  }

  if (mChargerFound) {
    ConfigureCharger();
  }

  ReadStatus(&St);
  DEBUG((EFI_D_INFO, "RT5033: battery %a, %u%%, %u mV, charger state %u\n",
         St.BatteryPresent ? "present" : "absent?", St.Capacity, St.VoltageMv, St.State));

  if (mChargerFound && mGaugeFound) {
    MonitorTick(NULL, NULL);
    Status = gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, MonitorTick, NULL, &mMonitorEvent);
    if (!EFI_ERROR(Status)) {
      gBS->SetTimer(mMonitorEvent, TimerPeriodic, MONITOR_PERIOD);
    }
    gBS->CreateEvent(EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY, OnExitBootServices, NULL, &mExitBsEvent);
  }

  return gBS->InstallMultipleProtocolInterfaces(&Handle, &gRt5033BatteryProtocolGuid, &mProtocol, NULL);
}
