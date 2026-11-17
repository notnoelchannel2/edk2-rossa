/*
 * Zinitix BT541 touchscreen driver (Samsung Galaxy Grand Prime and friends).
 *
 * Produces EFI_ABSOLUTE_POINTER_PROTOCOL so the UEFI console (ConSplitter)
 * and touch aware boot loaders (e.g. Windows Boot Manager) can use it.
 *
 * Register protocol and power-on sequence taken from the mainline Linux
 * driver (drivers/input/touchscreen/zinitix.c); the "no point exists ->
 * release everything" rule comes from Samsung's downstream bt541 driver.
 *
 * The chip talks I2C with 16-bit little endian register addresses. A read
 * must be a register write, a STOP, then a separate read transaction
 * (a repeated start does not work on this chip).
 *
 * INT is active low and stays asserted until CLEAR_INT_STATUS is sent, so
 * sampling it from a timer cannot miss events. Finger state is tracked per
 * slot because the chip only reports fingers that changed.
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

/* Registers / commands */
#define ZINITIX_SWRESET_CMD               0x0000
#define ZINITIX_CLEAR_INT_STATUS_CMD      0x0003
#define ZINITIX_TOUCH_MODE                0x0010
#define ZINITIX_CHIP_REVISION             0x0011
#define ZINITIX_FIRMWARE_VERSION          0x0012
#define ZINITIX_DATA_VERSION_REG          0x0013
#define ZINITIX_SUPPORTED_FINGER_NUM      0x0015
#define ZINITIX_INITIAL_TOUCH_MODE        0x0019
#define ZINITIX_POINT_STATUS_REG          0x0080
#define ZINITIX_BUTTON_SUPPORTED_NUM      0x00B0
#define ZINITIX_X_RESOLUTION              0x00C0
#define ZINITIX_Y_RESOLUTION              0x00C1
#define ZINITIX_INT_ENABLE_FLAG           0x00F0

/* Vendor power-on sequence */
#define ZINITIX_VENDOR_CMD_ENABLE         0xC000
#define ZINITIX_PROGRAM_START             0xC001
#define ZINITIX_NVM_INIT                  0xC002
#define ZINITIX_INTN_CLEAR                0xC004

/* Status register flags */
#define BIT_PT_CNT_CHANGE                 BIT0
#define BIT_DOWN                          BIT1
#define BIT_MOVE                          BIT2
#define BIT_UP                            BIT3
#define BIT_PT_EXIST                      BIT11

/* Per finger sub status */
#define SUB_BIT_EXIST                     BIT0
#define SUB_BIT_DOWN                      BIT1
#define SUB_BIT_MOVE                      BIT2
#define SUB_BIT_UP                        BIT3

#define DEFAULT_TOUCH_POINT_MODE          2
#define MAX_SUPPORTED_FINGER_NUM          5

/* Delays, with some margin over the Linux driver's 15 ms / 40 ms */
#define POWER_OFF_DELAY_MS                30
#define CHIP_ON_DELAY_MS                  30
#define FIRMWARE_ON_DELAY_MS              60

#define I2C_RETRY_COUNT                   3

/* Poll INT at 50Hz; read anyway every 25th tick (0.5 s) as a safety net */
#define TOUCH_POLL_PERIOD_100NS           (20 * 10000)
#define TOUCH_FORCED_POLL_TICKS           25

/* After this many failed reads in a row, power cycle and re-init the chip */
#define TOUCH_MAX_CONSECUTIVE_ERRORS      50
#define TOUCH_MAX_RECOVERIES              3

#define TOUCH_DEV_SIGNATURE SIGNATURE_32('B', 'T', '5', '4')

#pragma pack(1)
typedef struct {
  UINT16 X;
  UINT16 Y;
  UINT8  Width;
  UINT8  SubStatus;
  UINT8  MinorWidth;
  UINT8  Angle;
} ZINITIX_POINT;

typedef struct {
  UINT16        Status;
  UINT8         FingerMask;
  UINT8         TimeStamp;
  ZINITIX_POINT Point[MAX_SUPPORTED_FINGER_NUM];
} ZINITIX_EVENT;
#pragma pack()

typedef struct {
  BOOLEAN Down;
  UINT16  X;
  UINT16  Y;
} TOUCH_SLOT;

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
  UINT32                        PowerGpio;

  UINT16                        ChipRevision;
  UINT16                        FirmwareVersion;
  UINT16                        RegDataVersion;

  EFI_EVENT                     PollEvent;
  UINTN                         TickCount;
  UINTN                         ErrorCount;
  UINTN                         Recoveries;

  TOUCH_SLOT                    Slot[MAX_SUPPORTED_FINGER_NUM];

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

/* ---------------------------------------------------------------------- */
/* I2C helpers                                                            */
/* ---------------------------------------------------------------------- */

STATIC EFI_STATUS
ZinitixWrite (
  IN TOUCH_DEV    *Dev,
  IN CONST UINT8  *Buf,
  IN UINTN        Len
  )
{
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  UINTN      Retry;

  for (Retry = 0; Retry < I2C_RETRY_COUNT; Retry++) {
    Status = GpioI2cTransfer(&Dev->Bus, Dev->Address, Buf, Len, NULL, 0);
    if (!EFI_ERROR(Status)) {
      break;
    }
    MicroSecondDelay(100);
  }

  return Status;
}

STATIC EFI_STATUS
ZinitixWriteCmd (
  IN TOUCH_DEV  *Dev,
  IN UINT16     Reg
  )
{
  UINT8 Buf[2];

  Buf[0] = (UINT8)Reg;
  Buf[1] = (UINT8)(Reg >> 8);

  return ZinitixWrite(Dev, Buf, sizeof(Buf));
}

STATIC EFI_STATUS
ZinitixWriteU16 (
  IN TOUCH_DEV  *Dev,
  IN UINT16     Reg,
  IN UINT16     Value
  )
{
  UINT8 Buf[4];

  Buf[0] = (UINT8)Reg;
  Buf[1] = (UINT8)(Reg >> 8);
  Buf[2] = (UINT8)Value;
  Buf[3] = (UINT8)(Value >> 8);

  return ZinitixWrite(Dev, Buf, sizeof(Buf));
}

STATIC EFI_STATUS
ZinitixRead (
  IN  TOUCH_DEV  *Dev,
  IN  UINT16     Reg,
  OUT VOID       *Data,
  IN  UINTN      Len
  )
{
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  UINT8      RegLe[2];
  UINTN      Retry;

  RegLe[0] = (UINT8)Reg;
  RegLe[1] = (UINT8)(Reg >> 8);

  for (Retry = 0; Retry < I2C_RETRY_COUNT; Retry++) {
    /* Separate write and read transactions, see the header comment */
    Status = GpioI2cTransfer(&Dev->Bus, Dev->Address, RegLe, sizeof(RegLe), NULL, 0);
    if (!EFI_ERROR(Status)) {
      /* Linux zinitix_read_data(): DELAY_FOR_TRANSCATION between the two */
      MicroSecondDelay(50);
      Status = GpioI2cTransfer(&Dev->Bus, Dev->Address, NULL, 0, Data, Len);
      if (!EFI_ERROR(Status)) {
        break;
      }
    }
    MicroSecondDelay(100);
  }

  return Status;
}

STATIC UINT16
ZinitixReadU16 (
  IN TOUCH_DEV  *Dev,
  IN UINT16     Reg
  )
{
  UINT8 Data[2];

  if (EFI_ERROR(ZinitixRead(Dev, Reg, Data, sizeof(Data)))) {
    return 0xFFFF;
  }

  return (UINT16)(Data[0] | (Data[1] << 8));
}

/* ---------------------------------------------------------------------- */
/* Chip bring-up                                                          */
/* ---------------------------------------------------------------------- */

STATIC VOID
Bt541PowerCycle (
  IN TOUCH_DEV  *Dev
  )
{
  if (Dev->IntGpio != GPIO_NONE) {
    gGpioTlmm->SetFunction(Dev->IntGpio, 0);
    gGpioTlmm->SetDriveStrength(Dev->IntGpio, 2);
    gGpioTlmm->SetPull(Dev->IntGpio, GPIO_PULL_NONE);
    gGpioTlmm->DirectionInput(Dev->IntGpio);
  }

  if (Dev->PowerGpio != GPIO_NONE) {
    gGpioTlmm->SetFunction(Dev->PowerGpio, 0);
    gGpioTlmm->SetDriveStrength(Dev->PowerGpio, 2);
    gGpioTlmm->SetPull(Dev->PowerGpio, GPIO_PULL_NONE);

    /* Start from a known state whatever the bootloader did */
    gGpioTlmm->DirectionOutput(Dev->PowerGpio, 0);
    MicroSecondDelay(POWER_OFF_DELAY_MS * 1000);
    gGpioTlmm->DirectionOutput(Dev->PowerGpio, 1);
  }

  MicroSecondDelay(CHIP_ON_DELAY_MS * 1000);
}

STATIC EFI_STATUS
Bt541PowerOnSequence (
  IN TOUCH_DEV  *Dev
  )
{
  EFI_STATUS Status;

  Status = ZinitixWriteU16(Dev, ZINITIX_VENDOR_CMD_ENABLE, 0x0001);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "BT541: no response at 0x%02x (vendor cmd enable): %r\n", Dev->Address, Status));
    return Status;
  }
  MicroSecondDelay(10);

  Status = ZinitixWriteCmd(Dev, ZINITIX_INTN_CLEAR);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "BT541: intn clear failed: %r\n", Status));
    return Status;
  }
  MicroSecondDelay(10);

  Status = ZinitixWriteU16(Dev, ZINITIX_NVM_INIT, 0x0001);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "BT541: nvm init failed: %r\n", Status));
    return Status;
  }
  MicroSecondDelay(2 * 1000);

  Status = ZinitixWriteU16(Dev, ZINITIX_PROGRAM_START, 0x0001);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "BT541: program start failed: %r\n", Status));
    return Status;
  }
  MicroSecondDelay(FIRMWARE_ON_DELAY_MS * 1000);

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Bt541InitTouch (
  IN TOUCH_DEV  *Dev
  )
{
  EFI_STATUS Status;
  UINTN      Index;

  Status = ZinitixWriteCmd(Dev, ZINITIX_SWRESET_CMD);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "BT541: reset command failed: %r\n", Status));
    return Status;
  }

  Dev->ChipRevision    = ZinitixReadU16(Dev, ZINITIX_CHIP_REVISION);
  Dev->FirmwareVersion = ZinitixReadU16(Dev, ZINITIX_FIRMWARE_VERSION);
  Dev->RegDataVersion  = ZinitixReadU16(Dev, ZINITIX_DATA_VERSION_REG);
  DEBUG((EFI_D_ERROR, "BT541: chip revision %04x firmware %04x regdata %04x\n",
         Dev->ChipRevision, Dev->FirmwareVersion, Dev->RegDataVersion));

  Status = ZinitixWriteU16(Dev, ZINITIX_INT_ENABLE_FLAG, 0x0);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  /* Same values the Linux driver programs (touchscreen-size minus one) */
  Status = ZinitixWriteU16(Dev, ZINITIX_X_RESOLUTION, (UINT16)(FixedPcdGet32(PcdTouchMaxX) - 1));
  if (!EFI_ERROR(Status)) {
    Status = ZinitixWriteU16(Dev, ZINITIX_Y_RESOLUTION, (UINT16)(FixedPcdGet32(PcdTouchMaxY) - 1));
  }
  if (!EFI_ERROR(Status)) {
    Status = ZinitixWriteU16(Dev, ZINITIX_SUPPORTED_FINGER_NUM, MAX_SUPPORTED_FINGER_NUM);
  }
  if (!EFI_ERROR(Status)) {
    /* The capacitive menu/back keys are not used in UEFI */
    Status = ZinitixWriteU16(Dev, ZINITIX_BUTTON_SUPPORTED_NUM, 0);
  }
  if (!EFI_ERROR(Status)) {
    Status = ZinitixWriteU16(Dev, ZINITIX_INITIAL_TOUCH_MODE, DEFAULT_TOUCH_POINT_MODE);
  }
  if (!EFI_ERROR(Status)) {
    Status = ZinitixWriteU16(Dev, ZINITIX_TOUCH_MODE, DEFAULT_TOUCH_POINT_MODE);
  }
  if (!EFI_ERROR(Status)) {
    Status = ZinitixWriteU16(Dev, ZINITIX_INT_ENABLE_FLAG,
                             BIT_PT_CNT_CHANGE | BIT_DOWN | BIT_MOVE | BIT_UP);
  }
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "BT541: configuration failed: %r\n", Status));
    return Status;
  }

  /* Clear the event queue */
  for (Index = 0; Index < 10; Index++) {
    ZinitixWriteCmd(Dev, ZINITIX_CLEAR_INT_STATUS_CMD);
    MicroSecondDelay(10);
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Bt541Start (
  IN TOUCH_DEV  *Dev
  )
{
  EFI_STATUS Status;

  ZeroMem(Dev->Slot, sizeof(Dev->Slot));

  Bt541PowerCycle(Dev);
  GpioI2cInit(&Dev->Bus);

  Status = Bt541PowerOnSequence(Dev);
  if (!EFI_ERROR(Status)) {
    Status = Bt541InitTouch(Dev);
  }

  return Status;
}

/* ---------------------------------------------------------------------- */
/* Event handling                                                         */
/* ---------------------------------------------------------------------- */

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
Bt541UpdateState (
  IN TOUCH_DEV  *Dev
  )
{
  BOOLEAN Touching = FALSE;
  UINT64  X        = Dev->State.CurrentX;
  UINT64  Y        = Dev->State.CurrentY;
  UINTN   Index;

  /* The lowest finger that is down acts like a mouse */
  for (Index = 0; Index < MAX_SUPPORTED_FINGER_NUM; Index++) {
    if (Dev->Slot[Index].Down) {
      X        = Dev->Slot[Index].X;
      Y        = Dev->Slot[Index].Y;
      Touching = TRUE;
      TransformCoordinates(&X, &Y);
      break;
    }
  }

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
Bt541Recover (
  IN TOUCH_DEV  *Dev
  )
{
  EFI_STATUS Status;

  Dev->ErrorCount = 0;

  if (Dev->Recoveries >= TOUCH_MAX_RECOVERIES) {
    return;
  }
  Dev->Recoveries++;

  DEBUG((EFI_D_ERROR, "BT541: too many I2C errors, re-initializing (%u)\n", Dev->Recoveries));
  Status = Bt541Start(Dev);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "BT541: re-init failed: %r\n", Status));
  }

  Bt541UpdateState(Dev);
}

STATIC VOID
Bt541Poll (
  IN TOUCH_DEV  *Dev
  )
{
  EFI_STATUS    Status;
  ZINITIX_EVENT Event;
  UINT16        EventStatus;
  UINTN         Index;

  ZeroMem(&Event, sizeof(Event));
  Status = ZinitixRead(Dev, ZINITIX_POINT_STATUS_REG, &Event, sizeof(Event));
  /* Always ack, otherwise INT stays asserted */
  ZinitixWriteCmd(Dev, ZINITIX_CLEAR_INT_STATUS_CMD);

  if (EFI_ERROR(Status)) {
    if (++Dev->ErrorCount >= TOUCH_MAX_CONSECUTIVE_ERRORS) {
      Bt541Recover(Dev);
    }
    return;
  }
  Dev->ErrorCount = 0;

  EventStatus = Event.Status;
  if (EventStatus == 0 || EventStatus == 0xFFFF) {
    /* Nothing pending (or the bus floated high) */
    return;
  }

  for (Index = 0; Index < MAX_SUPPORTED_FINGER_NUM; Index++) {
    ZINITIX_POINT *P = &Event.Point[Index];

    if (!(Event.FingerMask & (1U << Index)) || !(P->SubStatus & SUB_BIT_EXIST)) {
      continue;
    }

    if (P->SubStatus & SUB_BIT_UP) {
      Dev->Slot[Index].Down = FALSE;
    } else if (P->SubStatus & (SUB_BIT_DOWN | SUB_BIT_MOVE)) {
      Dev->Slot[Index].Down = TRUE;
      Dev->Slot[Index].X    = P->X;
      Dev->Slot[Index].Y    = P->Y;
    }
  }

  /* No finger on the panel any more: drop anything whose UP we missed */
  if (!(EventStatus & BIT_PT_EXIST)) {
    for (Index = 0; Index < MAX_SUPPORTED_FINGER_NUM; Index++) {
      Dev->Slot[Index].Down = FALSE;
    }
  }

  Bt541UpdateState(Dev);
}

STATIC VOID
EFIAPI
TouchPollNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  TOUCH_DEV *Dev    = Context;
  BOOLEAN   Pending = FALSE;

  Dev->TickCount++;

  /* INT is active low and level-like (held until CLEAR_INT_STATUS) */
  if (Dev->IntGpio == GPIO_NONE || gGpioTlmm->Get(Dev->IntGpio) == 0) {
    Pending = TRUE;
  }
  if (Dev->TickCount % TOUCH_FORCED_POLL_TICKS == 0) {
    Pending = TRUE;
  }

  if (Pending) {
    Bt541Poll(Dev);
  }
}

/* ---------------------------------------------------------------------- */
/* EFI_ABSOLUTE_POINTER_PROTOCOL                                          */
/* ---------------------------------------------------------------------- */

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
  ZeroMem(Dev->Slot, sizeof(Dev->Slot));
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

/* Stop polling before the OS takes over the GPIOs */
STATIC EFI_EVENT mExitBootServicesEvent;

STATIC VOID
EFIAPI
TouchExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  TOUCH_DEV *Dev = Context;

  gBS->SetTimer(Dev->PollEvent, TimerCancel, 0);
}

EFI_STATUS
EFIAPI
Bt541TouchDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;
  TOUCH_DEV  *Dev;

  if (FixedPcdGet32(PcdTouchI2cSdaGpio) == GPIO_NONE ||
      FixedPcdGet32(PcdTouchI2cSclGpio) == GPIO_NONE) {
    return EFI_UNSUPPORTED;
  }

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
  Dev->PowerGpio        = FixedPcdGet32(PcdTouchPowerEnableGpio);

  Status = Bt541Start(Dev);
  if (EFI_ERROR(Status)) {
    /* Retry once from a fresh power cycle; the first attempt can race the LDO */
    Status = Bt541Start(Dev);
  }
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "BT541: initialization failed: %r\n", Status));
    goto Fail;
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

  Status = gBS->CreateEventEx(
      EVT_NOTIFY_SIGNAL, TPL_NOTIFY, TouchExitBootServices, Dev,
      &gEfiEventExitBootServicesGuid, &mExitBootServicesEvent);
  if (EFI_ERROR(Status)) {
    goto FailPollEvent;
  }

  Status = gBS->InstallMultipleProtocolInterfaces(
      &Dev->Handle,
      &gEfiAbsolutePointerProtocolGuid, &Dev->AbsPointer,
      &gEfiDevicePathProtocolGuid, &Dev->DevicePath,
      NULL);
  if (EFI_ERROR(Status)) {
    goto FailExitEvent;
  }

  DEBUG((EFI_D_ERROR, "BT541: ready\n"));
  return EFI_SUCCESS;

FailExitEvent:
  gBS->CloseEvent(mExitBootServicesEvent);
FailPollEvent:
  gBS->SetTimer(Dev->PollEvent, TimerCancel, 0);
  gBS->CloseEvent(Dev->PollEvent);
FailWaitEvent:
  gBS->CloseEvent(Dev->AbsPointer.WaitForInput);
Fail:
  FreePool(Dev);
  return Status;
}
