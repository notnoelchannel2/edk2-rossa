/*
 * Zinitix BT541 touchscreen driver (Galaxy A3 2015, a3ulte).
 *
 * The A3 uses a Zinitix BT541 on blsp_i2c5 (GPIO 18/19, address 0x20),
 * INT on GPIO 13 (active low, stays low until the interrupt status is
 * cleared), vcca from the vdd_tsp_a fixed regulator (enable GPIO 73) and
 * vdd from PM8916 L6, which the bootloader already left on.
 *
 * Produces EFI_ABSOLUTE_POINTER_PROTOCOL so the UEFI console (ConSplitter)
 * and touch aware boot loaders (e.g. Windows Boot Manager) can use it.
 *
 * Register protocol, power-on sequence and init values are taken from the
 * mainline Linux driver (drivers/input/touchscreen/zinitix.c) and the
 * a3u-eur device tree (msm8916-samsung-a3u-eur.dts):
 *   - registers are 16-bit little endian, values 16-bit little endian
 *   - a register read is a write of the register number, a STOP, and a
 *     separate read ("a single i2c_transfer() transaction does not work")
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

/* Register map (subset), from zinitix.c */
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

/* Vendor power-on sequence registers */
#define ZINITIX_VENDOR_CMD_ENABLE         0xC000
#define ZINITIX_PROGRAM_START             0xC001
#define ZINITIX_NVM_INIT                  0xC002
#define ZINITIX_INTN_CLEAR                0xC004

/* Interrupt & status register flags */
#define BIT_PT_CNT_CHANGE                 BIT0
#define BIT_DOWN                          BIT1
#define BIT_MOVE                          BIT2
#define BIT_UP                            BIT3
#define BIT_PT_EXIST                      BIT11

#define SUB_BIT_EXIST                     BIT0
#define SUB_BIT_DOWN                      BIT1
#define SUB_BIT_MOVE                      BIT2
#define SUB_BIT_UP                        BIT3

#define DEFAULT_TOUCH_POINT_MODE          2
#define MAX_SUPPORTED_FINGER_NUM          5

#define CHIP_ON_DELAY_MS                  15
#define FIRMWARE_ON_DELAY_MS              40
#define I2C_RETRY_COUNT                   3

/* 8 bytes per contact, 4 byte header: 44 bytes per report */
#define POINT_COORD_SIZE                  8
#define TOUCH_EVENT_SIZE                  (4 + POINT_COORD_SIZE * MAX_SUPPORTED_FINGER_NUM)

/* Poll INT at 50Hz; while touching, also read every 5th tick in case an UP report was lost */
#define TOUCH_POLL_PERIOD_100NS           (20 * 10000)
#define TOUCH_FORCED_POLL_TICKS           5

#define TOUCH_DEV_SIGNATURE SIGNATURE_32('Z', 'N', 'T', 'X')

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

  UINT16                        ChipRevision;
  UINT16                        FirmwareVersion;
  UINT16                        RegDataVersion;

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
ZinitixSend (
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
  }

  return Status;
}

STATIC EFI_STATUS
ZinitixWriteCmd (
  IN TOUCH_DEV  *Dev,
  IN UINT16     Reg
  )
{
  UINT8 Buf[2] = { (UINT8)Reg, (UINT8)(Reg >> 8) };

  return ZinitixSend(Dev, Buf, sizeof(Buf));
}

STATIC EFI_STATUS
ZinitixWriteU16 (
  IN TOUCH_DEV  *Dev,
  IN UINT16     Reg,
  IN UINT16     Value
  )
{
  UINT8 Buf[4] = { (UINT8)Reg, (UINT8)(Reg >> 8), (UINT8)Value, (UINT8)(Value >> 8) };

  return ZinitixSend(Dev, Buf, sizeof(Buf));
}

/* Register number, STOP, then a separate read transaction */
STATIC EFI_STATUS
ZinitixReadData (
  IN  TOUCH_DEV  *Dev,
  IN  UINT16     Reg,
  OUT UINT8      *Data,
  IN  UINTN      Len
  )
{
  UINT8      RegLe[2] = { (UINT8)Reg, (UINT8)(Reg >> 8) };
  EFI_STATUS Status   = EFI_DEVICE_ERROR;
  UINTN      Retry;

  for (Retry = 0; Retry < I2C_RETRY_COUNT; Retry++) {
    Status = GpioI2cTransfer(&Dev->Bus, Dev->Address, RegLe, sizeof(RegLe), NULL, 0);
    if (EFI_ERROR(Status)) {
      continue;
    }
    Status = GpioI2cTransfer(&Dev->Bus, Dev->Address, NULL, 0, Data, Len);
    if (!EFI_ERROR(Status)) {
      break;
    }
  }

  return Status;
}

STATIC UINT16
ZinitixReadU16 (
  IN TOUCH_DEV  *Dev,
  IN UINT16     Reg
  )
{
  UINT8 Val[2];

  if (EFI_ERROR(ZinitixReadData(Dev, Reg, Val, sizeof(Val)))) {
    return 0xFFFF;
  }

  return (UINT16)(Val[0] | (Val[1] << 8));
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
ZinitixPoll (
  IN TOUCH_DEV  *Dev,
  IN BOOLEAN    IntAsserted
  )
{
  UINT8      Event[TOUCH_EVENT_SIZE];
  UINT16     Status;
  UINT8      FingerMask;
  UINT8      *Point;
  UINT8      SubStatus;
  UINTN      Index;
  BOOLEAN    Touching = FALSE;
  UINT64     X = Dev->State.CurrentX;
  UINT64     Y = Dev->State.CurrentY;

  ZeroMem(Event, sizeof(Event));
  if (EFI_ERROR(ZinitixReadData(Dev, ZINITIX_POINT_STATUS_REG, Event, sizeof(Event)))) {
    goto Out;
  }

  Status     = (UINT16)(Event[0] | (Event[1] << 8));
  FingerMask = Event[2];

  /*
   * An empty status on a forced read (INT not asserted) carries no
   * information: keep the current state. With INT asserted an empty
   * status means nothing is touching any more.
   */
  if (Status == 0 && !IntAsserted) {
    return;
  }

  /* Only the first finger that is down is reported, it acts like a mouse */
  for (Index = 0; Index < MAX_SUPPORTED_FINGER_NUM; Index++) {
    if (!(FingerMask & (1U << Index))) {
      continue;
    }

    Point     = &Event[4 + Index * POINT_COORD_SIZE];
    SubStatus = Point[5];

    if (!(SubStatus & SUB_BIT_EXIST) || (SubStatus & SUB_BIT_UP) ||
        !(SubStatus & (SUB_BIT_DOWN | SUB_BIT_MOVE))) {
      continue;
    }

    X        = Point[0] | (Point[1] << 8);
    Y        = Point[2] | (Point[3] << 8);
    Touching = TRUE;
    TransformCoordinates(&X, &Y);
    break;
  }

  /*
   * A stationary finger produces no MOVE/DOWN sub-status, only PT_EXIST:
   * don't turn that into a release.
   */
  if (!Touching && (Status & BIT_PT_EXIST) &&
      (Dev->State.ActiveButtons & EFI_ABSP_TouchActive) && !(Status & BIT_UP)) {
    goto Out;
  }

  /*
   * A forced read (INT not asserted) may return the previous, already
   * cleared report: only trust it to end a touch whose UP was missed,
   * never to start or move one.
   */
  if (!IntAsserted && Touching) {
    return;
  }

  TouchUpdateState(Dev, Touching, X, Y);

Out:
  if (IntAsserted) {
    ZinitixWriteCmd(Dev, ZINITIX_CLEAR_INT_STATUS_CMD);
  }
}

STATIC VOID
EFIAPI
TouchPollNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  TOUCH_DEV *Dev = Context;
  BOOLEAN   IntAsserted;

  Dev->TickCount++;

  /* INT is active low and stays low until CLEAR_INT_STATUS */
  IntAsserted = (Dev->IntGpio == GPIO_NONE) || (gGpioTlmm->Get(Dev->IntGpio) == 0);

  /* Forced reads only matter while a touch is active (to catch a lost UP) */
  if (IntAsserted ||
      ((Dev->State.ActiveButtons & EFI_ABSP_TouchActive) &&
       (Dev->TickCount % TOUCH_FORCED_POLL_TICKS) == 0)) {
    ZinitixPoll(Dev, IntAsserted);
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
ZinitixPowerOn (
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

  /* zinitix.c waits CHIP_ON_DELAY after the regulators; be generous */
  MicroSecondDelay(CHIP_ON_DELAY_MS * 2 * 1000);
}

/* zinitix_send_power_on_sequence(): start the controller firmware */
STATIC EFI_STATUS
ZinitixPowerOnSequence (
  IN TOUCH_DEV  *Dev
  )
{
  EFI_STATUS Status;

  Status = ZinitixWriteU16(Dev, ZINITIX_VENDOR_CMD_ENABLE, 0x0001);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "Zinitix: no response at 0x%02x (vendor cmd enable)\n", Dev->Address));
    return EFI_NOT_FOUND;
  }
  MicroSecondDelay(10);

  Status = ZinitixWriteCmd(Dev, ZINITIX_INTN_CLEAR);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "Zinitix: power sequence failed (intn clear)\n"));
    return Status;
  }
  MicroSecondDelay(10);

  Status = ZinitixWriteU16(Dev, ZINITIX_NVM_INIT, 0x0001);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "Zinitix: power sequence failed (nvm init)\n"));
    return Status;
  }
  MicroSecondDelay(2 * 1000);

  Status = ZinitixWriteU16(Dev, ZINITIX_PROGRAM_START, 0x0001);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "Zinitix: power sequence failed (program start)\n"));
    return Status;
  }
  MicroSecondDelay(FIRMWARE_ON_DELAY_MS * 1000);

  return EFI_SUCCESS;
}

/* zinitix_init_touch(): reset, report resolution/fingers, enable interrupts */
STATIC EFI_STATUS
ZinitixInitTouch (
  IN TOUCH_DEV  *Dev
  )
{
  EFI_STATUS Status;
  UINTN      Index;

  Status = ZinitixWriteCmd(Dev, ZINITIX_SWRESET_CMD);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "Zinitix: reset command failed\n"));
    return Status;
  }

  Dev->ChipRevision    = ZinitixReadU16(Dev, ZINITIX_CHIP_REVISION);
  Dev->FirmwareVersion = ZinitixReadU16(Dev, ZINITIX_FIRMWARE_VERSION);
  Dev->RegDataVersion  = ZinitixReadU16(Dev, ZINITIX_DATA_VERSION_REG);
  DEBUG((EFI_D_ERROR, "Zinitix: chip revision %04x firmware %04x regdata %04x\n",
         Dev->ChipRevision, Dev->FirmwareVersion, Dev->RegDataVersion));

  Status = ZinitixWriteU16(Dev, ZINITIX_INT_ENABLE_FLAG, 0);
  if (!EFI_ERROR(Status)) {
    /* touchscreen_parse_properties(): max = size - 1 */
    Status = ZinitixWriteU16(Dev, ZINITIX_X_RESOLUTION, (UINT16)(FixedPcdGet32(PcdTouchMaxX) - 1));
  }
  if (!EFI_ERROR(Status)) {
    Status = ZinitixWriteU16(Dev, ZINITIX_Y_RESOLUTION, (UINT16)(FixedPcdGet32(PcdTouchMaxY) - 1));
  }
  if (!EFI_ERROR(Status)) {
    Status = ZinitixWriteU16(Dev, ZINITIX_SUPPORTED_FINGER_NUM, MAX_SUPPORTED_FINGER_NUM);
  }
  if (!EFI_ERROR(Status)) {
    /* The A3's capacitive keys are a separate MCU, not BT541 "icons" */
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
    DEBUG((EFI_D_ERROR, "Zinitix: configuring the controller failed: %r\n", Status));
    return Status;
  }

  /* Clear the event queue */
  for (Index = 0; Index < 10; Index++) {
    ZinitixWriteCmd(Dev, ZINITIX_CLEAR_INT_STATUS_CMD);
    MicroSecondDelay(10);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
ZinitixTouchDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;
  TOUCH_DEV  *Dev;

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

  ZinitixPowerOn(Dev);
  GpioI2cInit(&Dev->Bus);

  Status = ZinitixPowerOnSequence(Dev);
  if (EFI_ERROR(Status)) {
    goto Fail;
  }

  Status = ZinitixInitTouch(Dev);
  if (EFI_ERROR(Status)) {
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
