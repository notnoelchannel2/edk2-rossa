/*
 * Imagis IST30xx touchscreen driver.
 *
 * Two register protocols are supported, picked at probe time from the chip id:
 *  - IST3038C/IST3032C/IST3038H ("HIB" protocol B), e.g. Galaxy J5 2016.
 *  - IST30xx/IST3038 (legacy, status/coordinates streamed from register 0x20),
 *    e.g. Galaxy Core Prime LTE (rossa) and Galaxy Grand Prime (fortuna).
 *
 * Produces EFI_ABSOLUTE_POINTER_PROTOCOL so the UEFI console (ConSplitter)
 * and touch aware boot loaders (e.g. Windows Boot Manager) can use it.
 *
 * Register protocol taken from the mainline Linux driver
 * (drivers/input/touchscreen/imagis.c).
 */

#include <Library/LKEnvLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/QcomGpioTlmmLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/AbsolutePointer.h>
#include <Protocol/DevicePath.h>

#include <Library/GpioI2cLib.h>

#define GPIO_NONE 0xFFFFFFFF

#define IST3038C_HIB_ACCESS          (0x800BU << 16)
#define IST3038C_DIRECT_ACCESS       BIT31
#define IST3038C_REG_CHIPID          (0x40001000U | IST3038C_DIRECT_ACCESS)
#define IST3038C_REG_HIB_BASE        0x30000100U
#define IST3038C_REG_INTR_MESSAGE    (IST3038C_REG_HIB_BASE | IST3038C_HIB_ACCESS | 0x4)
#define IST3038C_REG_TOUCH_COORD     (IST3038C_REG_HIB_BASE | IST3038C_HIB_ACCESS | 0x8)
#define IST3038C_WHOAMI              0x38c
#define IST3032C_WHOAMI              0x32c
#define IST3038H_WHOAMI              0x38d
#define IST3038C_CHIP_ON_DELAY_MS    60
#define IST3038C_I2C_RETRY_COUNT     3
#define IST3038C_MAX_FINGER_NUM      10

#define IST3038C_X(v)                (((v) >> 12) & 0xFFF)
#define IST3038C_Y(v)                ((v) & 0xFFF)
#define IST3038C_FINGER_COUNT(v)     (((v) >> 12) & 0xF)
#define IST3038C_FINGER_STATUS(v)    ((v) & 0x3FF)

/*
 * Legacy IST30xx protocol (Samsung imagis_core3 driver, mainline "imagis,ist3038"):
 * every read of register 0x20 returns the next word of the current report,
 * first the interrupt message, then one word per finger.
 */
#define IST30XX_REG_STATUS           0x20U
#define IST30XX_REG_CHIPID           (0x40000000U | IST3038C_DIRECT_ACCESS)
#define IST30XX_WHOAMI               0x30003000
#define IST30XXA_WHOAMI              0x300a300a
#define IST30XXB_WHOAMI              0x300b300b
#define IST3038_WHOAMI               0x30383038
#define IST30XXB_ALT_WHOAMI          0x0003000B  /* vendor driver treats it as IST30XXB */
#define IST30XX_CMD_START_SCAN       0x0BU
#define IST30XX_START_SCAN_DELAY_MS  100
#define IST30XX_INTR_STATUS          0x00000C00U
#define IST30XX_MSG_IC_EXCEPTION     0xE11CE970U
#define IST30XX_FINGER_ID(v)         (((v) >> 28) & 0xF)

typedef enum {
  ImagisProtocolC,       /* IST3038C family */
  ImagisProtocolLegacy,  /* IST30xx / IST3038 */
} IMAGIS_PROTOCOL;

/* Poll at 50Hz, and read unconditionally every 5th tick in case an INT edge was missed */
#define TOUCH_POLL_PERIOD_100NS      (20 * 10000)
#define TOUCH_FORCED_POLL_TICKS      5

#define TOUCH_DEV_SIGNATURE SIGNATURE_32('I', 'S', 'T', 'C')

typedef struct {
  VENDOR_DEVICE_PATH       Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} TOUCH_DEVICE_PATH;

typedef struct {
  UINT32                        Signature;
  EFI_HANDLE                    Handle;
  EFI_ABSOLUTE_POINTER_PROTOCOL AbsPointer;
  EFI_ABSOLUTE_POINTER_MODE     Mode;
  TOUCH_DEVICE_PATH             DevicePath;

  GPIO_I2C_BUS                  Bus;
  UINT8                         Address;
  UINT32                        IntGpio;
  IMAGIS_PROTOCOL               Protocol;

  EFI_EVENT                     PollEvent;
  UINTN                         TickCount;

  EFI_ABSOLUTE_POINTER_STATE    State;
  BOOLEAN                       StateChanged;
} TOUCH_DEV;

#define TOUCH_DEV_FROM_ABSPOINTER(a) CR(a, TOUCH_DEV, AbsPointer, TOUCH_DEV_SIGNATURE)

STATIC TOUCH_DEVICE_PATH mTouchDevicePathTemplate = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8),
      },
    },
    EFI_CALLER_ID_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0},
  },
};

STATIC EFI_STATUS
Ist3038cReadReg (
  IN  TOUCH_DEV  *Dev,
  IN  UINT32     Reg,
  OUT UINT32     *Value
  )
{
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  UINT8      RegBe[4];
  UINT8      Data[4];
  UINTN      Retry;

  RegBe[0] = (UINT8)(Reg >> 24);
  RegBe[1] = (UINT8)(Reg >> 16);
  RegBe[2] = (UINT8)(Reg >> 8);
  RegBe[3] = (UINT8)Reg;

  for (Retry = 0; Retry < IST3038C_I2C_RETRY_COUNT; Retry++) {
    Status = GpioI2cTransfer(&Dev->Bus, Dev->Address, RegBe, sizeof(RegBe), Data, sizeof(Data));
    if (!EFI_ERROR(Status)) {
      *Value = ((UINT32)Data[0] << 24) | ((UINT32)Data[1] << 16) |
               ((UINT32)Data[2] << 8) | Data[3];
      return EFI_SUCCESS;
    }
  }

  return Status;
}

/* Legacy protocol: write a 32-bit command with a 32-bit argument */
STATIC EFI_STATUS
Ist30xxWriteCmd (
  IN TOUCH_DEV  *Dev,
  IN UINT32     Cmd,
  IN UINT32     Value
  )
{
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  UINT8      Buf[8];
  UINTN      Retry;

  Buf[0] = (UINT8)(Cmd >> 24);
  Buf[1] = (UINT8)(Cmd >> 16);
  Buf[2] = (UINT8)(Cmd >> 8);
  Buf[3] = (UINT8)Cmd;
  Buf[4] = (UINT8)(Value >> 24);
  Buf[5] = (UINT8)(Value >> 16);
  Buf[6] = (UINT8)(Value >> 8);
  Buf[7] = (UINT8)Value;

  for (Retry = 0; Retry < IST3038C_I2C_RETRY_COUNT; Retry++) {
    Status = GpioI2cTransfer(&Dev->Bus, Dev->Address, Buf, sizeof(Buf), NULL, 0);
    if (!EFI_ERROR(Status)) {
      break;
    }
  }

  return Status;
}

/* Legacy protocol: read Count big endian words starting at register 0x20 */
STATIC EFI_STATUS
Ist30xxReadStatus (
  IN  TOUCH_DEV  *Dev,
  OUT UINT32     *Words,
  IN  UINTN      Count
  )
{
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  UINT8      RegBe[4] = { 0, 0, 0, (UINT8)IST30XX_REG_STATUS };
  UINT8      Data[4 * (IST3038C_MAX_FINGER_NUM + 1)];
  UINTN      Retry;
  UINTN      Index;

  if (Count == 0 || Count > IST3038C_MAX_FINGER_NUM + 1) {
    return EFI_INVALID_PARAMETER;
  }

  for (Retry = 0; Retry < IST3038C_I2C_RETRY_COUNT; Retry++) {
    Status = GpioI2cTransfer(&Dev->Bus, Dev->Address, RegBe, sizeof(RegBe), Data, Count * 4);
    if (!EFI_ERROR(Status)) {
      for (Index = 0; Index < Count; Index++) {
        Words[Index] = ((UINT32)Data[Index * 4] << 24) | ((UINT32)Data[Index * 4 + 1] << 16) |
                       ((UINT32)Data[Index * 4 + 2] << 8) | Data[Index * 4 + 3];
      }
      return EFI_SUCCESS;
    }
  }

  return Status;
}

STATIC VOID
TransformCoordinates (
  IN OUT UINT64  *X,
  IN OUT UINT64  *Y
  )
{
  UINT64 Tmp;

  if (FixedPcdGetBool(PcdTouchSwapXY)) {
    Tmp = *X;
    *X  = *Y;
    *Y  = Tmp;
  }

  if (*X > FixedPcdGet32(PcdTouchMaxX) - 1) {
    *X = FixedPcdGet32(PcdTouchMaxX) - 1;
  }
  if (*Y > FixedPcdGet32(PcdTouchMaxY) - 1) {
    *Y = FixedPcdGet32(PcdTouchMaxY) - 1;
  }

  if (FixedPcdGetBool(PcdTouchInvertX)) {
    *X = FixedPcdGet32(PcdTouchMaxX) - 1 - *X;
  }
  if (FixedPcdGetBool(PcdTouchInvertY)) {
    *Y = FixedPcdGet32(PcdTouchMaxY) - 1 - *Y;
  }
}

STATIC VOID
TouchUpdateState (
  IN TOUCH_DEV  *Dev,
  IN BOOLEAN    Touching,
  IN UINT64     X,
  IN UINT64     Y
  )
{
  if (Touching != ((Dev->State.ActiveButtons & EFI_ABSP_TouchActive) != 0) ||
      X != Dev->State.CurrentX || Y != Dev->State.CurrentY) {
    Dev->State.CurrentX      = X;
    Dev->State.CurrentY      = Y;
    Dev->State.CurrentZ      = 0;
    Dev->State.ActiveButtons = Touching ? EFI_ABSP_TouchActive : 0;
    Dev->StateChanged        = TRUE;
  }
}

STATIC VOID
Ist30xxPoll (
  IN TOUCH_DEV  *Dev
  )
{
  UINT32     Words[IST3038C_MAX_FINGER_NUM + 1];
  UINT32     Message;
  UINTN      FingerCount;
  UINTN      FingerStatus;
  UINTN      Index;
  UINTN      Id;
  BOOLEAN    Touching = FALSE;
  UINT64     X = Dev->State.CurrentX;
  UINT64     Y = Dev->State.CurrentY;

  if (EFI_ERROR(Ist30xxReadStatus(Dev, &Message, 1))) {
    return;
  }

  /* No report, unknown command, firmware exception or a calibration message */
  if (Message == 0 || Message == 0xFFFFFFFF || Message == IST30XX_MSG_IC_EXCEPTION ||
      (Message & IST30XX_INTR_STATUS) != IST30XX_INTR_STATUS) {
    return;
  }

  FingerCount  = IST3038C_FINGER_COUNT(Message);
  FingerStatus = IST3038C_FINGER_STATUS(Message);
  if (FingerCount > IST3038C_MAX_FINGER_NUM) {
    return;
  }

  if (FingerCount != 0) {
    /* Drain the whole report in one burst like the vendor driver does */
    if (EFI_ERROR(Ist30xxReadStatus(Dev, Words, FingerCount))) {
      return;
    }

    /* Only the first finger that is down is reported, it acts like a mouse */
    for (Index = 0; Index < FingerCount; Index++) {
      Id = IST30XX_FINGER_ID(Words[Index]);
      if (Id == 0 || !(FingerStatus & (1U << (Id - 1)))) {
        continue;
      }

      X        = IST3038C_X(Words[Index]);
      Y        = IST3038C_Y(Words[Index]);
      Touching = TRUE;
      TransformCoordinates(&X, &Y);
      break;
    }
  }

  TouchUpdateState(Dev, Touching, X, Y);
}

STATIC VOID
Ist3038cPoll (
  IN TOUCH_DEV  *Dev
  )
{
  EFI_STATUS Status;
  UINT32     Message;
  UINT32     Coord;
  UINTN      FingerCount;
  UINTN      FingerStatus;
  UINTN      Index;
  BOOLEAN    Touching = FALSE;
  UINT64     X = Dev->State.CurrentX;
  UINT64     Y = Dev->State.CurrentY;

  Status = Ist3038cReadReg(Dev, IST3038C_REG_INTR_MESSAGE, &Message);
  if (EFI_ERROR(Status)) {
    return;
  }

  FingerCount  = IST3038C_FINGER_COUNT(Message);
  FingerStatus = IST3038C_FINGER_STATUS(Message);
  if (FingerCount > IST3038C_MAX_FINGER_NUM) {
    return;
  }

  /* Only the first finger that is down is reported, it acts like a mouse */
  for (Index = 0; Index < FingerCount; Index++) {
    if (!(FingerStatus & (1U << Index))) {
      continue;
    }

    Status = Ist3038cReadReg(Dev, IST3038C_REG_TOUCH_COORD + (Index * 4), &Coord);
    if (EFI_ERROR(Status)) {
      return;
    }

    X        = IST3038C_X(Coord);
    Y        = IST3038C_Y(Coord);
    Touching = TRUE;
    TransformCoordinates(&X, &Y);
    break;
  }

  TouchUpdateState(Dev, Touching, X, Y);
}

STATIC VOID
EFIAPI
TouchPollNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  TOUCH_DEV *Dev     = Context;
  BOOLEAN   Pending  = FALSE;

  Dev->TickCount++;

  /* INT is active low; keep reading while a finger is down so we see the release */
  if (Dev->IntGpio == GPIO_NONE || gGpioTlmm->Get(Dev->IntGpio) == 0) {
    Pending = TRUE;
  }
  if (Dev->State.ActiveButtons & EFI_ABSP_TouchActive) {
    Pending = TRUE;
  }
  if (Dev->TickCount % TOUCH_FORCED_POLL_TICKS == 0) {
    Pending = TRUE;
  }

  if (Pending) {
    if (Dev->Protocol == ImagisProtocolLegacy) {
      Ist30xxPoll(Dev);
    } else {
      Ist3038cPoll(Dev);
    }
  }
}

STATIC EFI_STATUS
EFIAPI
TouchReset (
  IN EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN BOOLEAN                        ExtendedVerification
  )
{
  TOUCH_DEV *Dev = TOUCH_DEV_FROM_ABSPOINTER(This);
  EFI_TPL   OldTpl;

  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  ZeroMem(&Dev->State, sizeof(Dev->State));
  Dev->StateChanged = FALSE;
  gBS->RestoreTPL(OldTpl);

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
TouchGetState (
  IN     EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN OUT EFI_ABSOLUTE_POINTER_STATE     *State
  )
{
  TOUCH_DEV  *Dev = TOUCH_DEV_FROM_ABSPOINTER(This);
  EFI_TPL    OldTpl;
  EFI_STATUS Status = EFI_NOT_READY;

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  if (Dev->StateChanged) {
    CopyMem(State, &Dev->State, sizeof(*State));
    Dev->StateChanged = FALSE;
    Status            = EFI_SUCCESS;
  }
  gBS->RestoreTPL(OldTpl);

  return Status;
}

STATIC VOID
EFIAPI
TouchWaitForInput (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  TOUCH_DEV *Dev = Context;

  if (Dev->StateChanged) {
    gBS->SignalEvent(Event);
  }
}

STATIC VOID
Ist3038cPowerOn (
  IN TOUCH_DEV  *Dev
  )
{
  UINT32 PowerGpio = FixedPcdGet32(PcdTouchPowerEnableGpio);

  if (PowerGpio != GPIO_NONE) {
    gGpioTlmm->SetFunction(PowerGpio, 0);
    gGpioTlmm->SetDriveStrength(PowerGpio, 2);
    gGpioTlmm->SetPull(PowerGpio, GPIO_PULL_NONE);
    gGpioTlmm->DirectionOutput(PowerGpio, 1);
  }

  if (Dev->IntGpio != GPIO_NONE) {
    gGpioTlmm->SetFunction(Dev->IntGpio, 0);
    gGpioTlmm->SetDriveStrength(Dev->IntGpio, 2);
    gGpioTlmm->SetPull(Dev->IntGpio, GPIO_PULL_NONE);
    gGpioTlmm->DirectionInput(Dev->IntGpio);
  }

  MicroSecondDelay(IST3038C_CHIP_ON_DELAY_MS * 1000);
}

STATIC BOOLEAN
IsLegacyChipId (
  IN UINT32  ChipId
  )
{
  return ChipId == IST3038_WHOAMI || ChipId == IST30XX_WHOAMI ||
         ChipId == IST30XXA_WHOAMI || ChipId == IST30XXB_WHOAMI ||
         ChipId == IST30XXB_ALT_WHOAMI;
}

STATIC BOOLEAN
IsCChipId (
  IN UINT32  ChipId
  )
{
  return ChipId == IST3038C_WHOAMI || ChipId == IST3032C_WHOAMI ||
         ChipId == IST3038H_WHOAMI;
}

/*
 * Identify the controller. Both chip id registers are plain reads, so trying
 * the "wrong" one first is harmless; PcdTouchImagisLegacy only decides the
 * order and the protocol used when neither id is recognized.
 */
STATIC EFI_STATUS
Ist30xxProbe (
  IN TOUCH_DEV  *Dev
  )
{
  BOOLEAN    PreferLegacy = FixedPcdGetBool(PcdTouchImagisLegacy);
  UINT32     Regs[2];
  UINT32     ChipId;
  BOOLEAN    Responded = FALSE;
  EFI_STATUS Status;
  UINTN      Index;

  Regs[0] = PreferLegacy ? IST30XX_REG_CHIPID : IST3038C_REG_CHIPID;
  Regs[1] = PreferLegacy ? IST3038C_REG_CHIPID : IST30XX_REG_CHIPID;

  for (Index = 0; Index < ARRAY_SIZE(Regs); Index++) {
    Status = Ist3038cReadReg(Dev, Regs[Index], &ChipId);
    if (EFI_ERROR(Status)) {
      continue;
    }
    Responded = TRUE;

    if (Regs[Index] == IST30XX_REG_CHIPID && IsLegacyChipId(ChipId)) {
      Dev->Protocol = ImagisProtocolLegacy;
      DEBUG((EFI_D_ERROR, "IST30xx: found chip id 0x%08x (legacy protocol)\n", ChipId));
      return EFI_SUCCESS;
    }
    if (Regs[Index] == IST3038C_REG_CHIPID && IsCChipId(ChipId)) {
      Dev->Protocol = ImagisProtocolC;
      DEBUG((EFI_D_ERROR, "IST30xx: found chip id 0x%x (IST3038C protocol)\n", ChipId));
      return EFI_SUCCESS;
    }
    DEBUG((EFI_D_ERROR, "IST30xx: reg 0x%08x: unexpected chip id 0x%08x\n", Regs[Index], ChipId));
  }

  if (!Responded) {
    DEBUG((EFI_D_ERROR, "IST30xx: no response at 0x%02x\n", Dev->Address));
    return EFI_NOT_FOUND;
  }

  /* Not fatal, other IST30xx parts speak one of the two protocols */
  Dev->Protocol = PreferLegacy ? ImagisProtocolLegacy : ImagisProtocolC;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Ist3038cTouchDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;
  TOUCH_DEV  *Dev;
  UINT32     ChipId;

  Dev = AllocateZeroPool(sizeof(*Dev));
  if (Dev == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Dev->Signature        = TOUCH_DEV_SIGNATURE;
  Dev->Bus.SdaGpio      = FixedPcdGet32(PcdTouchI2cSdaGpio);
  Dev->Bus.SclGpio      = FixedPcdGet32(PcdTouchI2cSclGpio);
  Dev->Bus.HalfPeriodUs = 5; /* ~100kHz */
  Dev->Address          = FixedPcdGet8(PcdTouchI2cAddress);
  Dev->IntGpio          = FixedPcdGet32(PcdTouchIntGpio);

  Ist3038cPowerOn(Dev);
  GpioI2cInit(&Dev->Bus);

  Status = Ist30xxProbe(Dev);
  if (EFI_ERROR(Status)) {
    goto Fail;
  }

  if (Dev->Protocol == ImagisProtocolLegacy) {
    /* The vendor driver (re)starts scanning after every power up; harmless if already running */
    if (EFI_ERROR(Ist30xxWriteCmd(Dev, IST30XX_CMD_START_SCAN, 0))) {
      DEBUG((EFI_D_ERROR, "IST30xx: start scan command failed\n"));
    }
    MicroSecondDelay(IST30XX_START_SCAN_DELAY_MS * 1000);
  }

  Dev->Mode.AbsoluteMinX = 0;
  Dev->Mode.AbsoluteMinY = 0;
  Dev->Mode.AbsoluteMinZ = 0;
  Dev->Mode.AbsoluteMaxX = FixedPcdGet32(PcdTouchMaxX);
  Dev->Mode.AbsoluteMaxY = FixedPcdGet32(PcdTouchMaxY);
  Dev->Mode.AbsoluteMaxZ = 0;
  Dev->Mode.Attributes   = 0;

  Dev->AbsPointer.Reset    = TouchReset;
  Dev->AbsPointer.GetState = TouchGetState;
  Dev->AbsPointer.Mode     = &Dev->Mode;

  CopyMem(&Dev->DevicePath, &mTouchDevicePathTemplate, sizeof(Dev->DevicePath));

  Status = gBS->CreateEvent(
      EVT_NOTIFY_WAIT, TPL_NOTIFY, TouchWaitForInput, Dev,
      &Dev->AbsPointer.WaitForInput);
  if (EFI_ERROR(Status)) {
    goto Fail;
  }

  Status = gBS->CreateEvent(
      EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_NOTIFY, TouchPollNotify, Dev,
      &Dev->PollEvent);
  if (EFI_ERROR(Status)) {
    goto FailWaitEvent;
  }

  Status = gBS->SetTimer(Dev->PollEvent, TimerPeriodic, TOUCH_POLL_PERIOD_100NS);
  if (EFI_ERROR(Status)) {
    goto FailPollEvent;
  }

  Status = gBS->InstallMultipleProtocolInterfaces(
      &Dev->Handle,
      &gEfiAbsolutePointerProtocolGuid, &Dev->AbsPointer,
      &gEfiDevicePathProtocolGuid, &Dev->DevicePath,
      NULL);
  if (EFI_ERROR(Status)) {
    goto FailPollEvent;
  }

  return EFI_SUCCESS;

FailPollEvent:
  gBS->CloseEvent(Dev->PollEvent);
FailWaitEvent:
  gBS->CloseEvent(Dev->AbsPointer.WaitForInput);
Fail:
  FreePool(Dev);
  return Status;
}
