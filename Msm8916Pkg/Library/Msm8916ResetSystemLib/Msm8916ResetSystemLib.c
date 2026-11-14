/** @file
  ResetSystemLib for MSM8916 (Samsung TrustZone without PSCI).

  Reset:    PMIC PON PS_HOLD reset type = hard reset, then drop PS_HOLD.
  Shutdown: PMIC PON PS_HOLD reset type = shutdown,   then drop PS_HOLD.
  (Qualcomm LK: reboot_device() / shutdown_device(), pm8x41_reset_configure.)

  The PMIC is reached through the v2 PMIC arbiter: the PON peripheral
  (SID 0, PID 0x08) is looked up in the ppid -> channel table in the arbiter
  core, then written through that channel.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/ResetSystemLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>

/* MSM8916 */
#define MPM2_MPM_PS_HOLD          0x004AB000UL
#define SPMI_CHNLS_OFFSET         0x00400000UL  /* PcdSpmiBaseAddress + this */
#define SPMI_MAX_CHANNELS         128

/* v2 arbiter: core table, channel registers */
#define ARB_REG_CHNL(n)           (0x800 + 4 * (n))
#define CHNL_CMD                  0x00
#define CHNL_CONFIG               0x04
#define CHNL_STATUS               0x08
#define CHNL_WDATA0               0x10
#define CHNL_STRIDE               0x8000UL
#define CMD_OPCODE_EXT_WRITE_LONG 0x00
#define CMD_OPCODE_SHIFT          27
#define CMD_OFFSET_SHIFT          4
#define STATUS_DONE               BIT0

/* PM8916 PON (SID 0, PID 0x08) */
#define PON_SID                   0
#define PON_PID                   0x08
#define PON_PS_HOLD_RESET_CTL     0x5A
#define PON_PS_HOLD_RESET_CTL2    0x5B
#define   PS_HOLD_RESET_EN        BIT7
#define PON_PSHOLD_WARM_RESET     0x1
#define PON_PSHOLD_SHUTDOWN       0x4
#define PON_PSHOLD_HARD_RESET     0x7

STATIC UINTN      mPsHold;          /* MPM2_MPM_PS_HOLD (virtual after SetVirtualAddressMap) */
STATIC UINTN      mPonChannel;      /* channel register block of the PON peripheral */
STATIC BOOLEAN    mPonFound;
STATIC EFI_EVENT  mVirtualAddressChangeEvent;

/* Short busy wait without TimerLib (usable at runtime) */
STATIC
VOID
SpinDelay (
  IN UINTN  Loops
  )
{
  volatile UINTN  Count;

  for (Count = 0; Count < Loops; Count++) {
  }
}

STATIC
BOOLEAN
PonWrite (
  IN UINT8  Offset,
  IN UINT8  Value
  )
{
  UINTN  Timeout;

  if (!mPonFound) {
    return FALSE;
  }

  MmioWrite32 (mPonChannel + CHNL_CONFIG, 0);          /* no IRQ */
  MmioWrite32 (mPonChannel + CHNL_WDATA0, Value);
  MmioWrite32 (
    mPonChannel + CHNL_CMD,
    (CMD_OPCODE_EXT_WRITE_LONG << CMD_OPCODE_SHIFT) | ((UINT32)Offset << CMD_OFFSET_SHIFT) | 0 /* 1 byte */
    );

  for (Timeout = 0; Timeout < 100000; Timeout++) {
    UINT32  Status = MmioRead32 (mPonChannel + CHNL_STATUS);
    if (Status != 0) {
      return Status == STATUS_DONE;
    }
  }

  return FALSE;
}

STATIC
VOID
PsHoldReset (
  IN UINT8  ResetType
  )
{
  /* pm8x41_reset_configure(): disable, set type, enable */
  if (PonWrite (PON_PS_HOLD_RESET_CTL2, 0)) {
    SpinDelay (300000);
    PonWrite (PON_PS_HOLD_RESET_CTL, ResetType);
    PonWrite (PON_PS_HOLD_RESET_CTL2, PS_HOLD_RESET_EN);
  }

  /* Drop PS_HOLD: the PMIC now does what PON_PS_HOLD_RESET_CTL says */
  if (mPsHold != 0) {
    MmioWrite32 (mPsHold, 0);
  }

  for ( ; ; ) {
    CpuDeadLoop ();
  }
}

VOID
EFIAPI
ResetCold (
  VOID
  )
{
  PsHoldReset (PON_PSHOLD_HARD_RESET);
}

VOID
EFIAPI
ResetWarm (
  VOID
  )
{
  /* A warm PMIC reset keeps the reboot reason; plain reboot is a hard reset in LK */
  PsHoldReset (PON_PSHOLD_HARD_RESET);
}

VOID
EFIAPI
ResetShutdown (
  VOID
  )
{
  PsHoldReset (PON_PSHOLD_SHUTDOWN);
}

VOID
EFIAPI
ResetPlatformSpecific (
  IN UINTN  DataSize,
  IN VOID   *ResetData
  )
{
  ResetCold ();
}

VOID
EFIAPI
ResetSystem (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  switch (ResetType) {
    case EfiResetWarm:
      ResetWarm ();
      break;
    case EfiResetCold:
      ResetCold ();
      break;
    case EfiResetShutdown:
      ResetShutdown ();
      return;
    case EfiResetPlatformSpecific:
      ResetPlatformSpecific (DataSize, ResetData);
      return;
    default:
      return;
  }
}

STATIC
VOID
EFIAPI
VirtualAddressChange (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  /* Both live in the runtime "Peripherals" region of the memory map */
  if (EFI_ERROR (EfiConvertPointer (0, (VOID **)&mPsHold))) {
    mPsHold = 0;
  }
  if (mPonFound && EFI_ERROR (EfiConvertPointer (0, (VOID **)&mPonChannel))) {
    mPonFound = FALSE;
  }
}

/*
 * The MMIO this library touches after ExitBootServices must be in the UEFI
 * memory map with EFI_MEMORY_RUNTIME, or the OS doesn't map it and
 * EfiConvertPointer fails (the "Peripherals" region is plain reserved
 * memory). Without this, Windows' reboot wrote to address 0 and crashed
 * with KMODE_EXCEPTION_NOT_HANDLED. 64 KB granules, as AArch64 requires.
 */
STATIC
VOID
MarkRuntimeMmio (
  IN UINTN  Base,
  IN UINTN  Length
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;
  EFI_STATUS                       Status;
  UINTN                            End = ALIGN_VALUE (Base + Length, SIZE_64KB);

  Base   &= ~((UINTN)SIZE_64KB - 1);
  Length  = End - Base;

  Status = gDS->GetMemorySpaceDescriptor (Base, &Desc);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Msm8916ResetSystemLib: no GCD entry for 0x%lx: %r\n", (UINT64)Base, Status));
    return;
  }
  if (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Base, Length, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Msm8916ResetSystemLib: AddMemorySpace 0x%lx: %r\n", (UINT64)Base, Status));
      return;
    }
  } else {
    Status = gDS->SetMemorySpaceCapabilities (Base, Length, Desc.Capabilities | EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Msm8916ResetSystemLib: capabilities 0x%lx: %r\n", (UINT64)Base, Status));
    }
  }
  Status = gDS->SetMemorySpaceAttributes (Base, Length, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
  DEBUG ((DEBUG_INFO, "Msm8916ResetSystemLib: runtime MMIO 0x%lx+0x%lx: %r\n", (UINT64)Base, (UINT64)Length, Status));
}

EFI_STATUS
EFIAPI
Msm8916ResetSystemLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINTN   Core  = (UINTN)FixedPcdGet64 (PcdPmicArbCoreAddress);
  UINTN   Chnls = (UINTN)FixedPcdGet64 (PcdSpmiBaseAddress) + SPMI_CHNLS_OFFSET;
  UINTN   Index;
  UINT32  Reg;

  mPsHold = MPM2_MPM_PS_HOLD;

  /* Find the arbiter channel that owns the PON peripheral */
  if (Core != 0) {
    for (Index = 0; Index < SPMI_MAX_CHANNELS; Index++) {
      Reg = MmioRead32 (Core + ARB_REG_CHNL (Index));
      if ((((Reg >> 16) & 0xF) == PON_SID) && (((Reg >> 8) & 0xFF) == PON_PID)) {
        mPonChannel = Chnls + Index * CHNL_STRIDE;
        mPonFound   = TRUE;
        break;
      }
    }
  }

  MarkRuntimeMmio (mPsHold, 4);
  if (mPonFound) {
    MarkRuntimeMmio (mPonChannel, CHNL_STRIDE);
  }

  DEBUG ((DEBUG_INFO, "Msm8916ResetSystemLib: PON channel %a (0x%lx)\n",
          mPonFound ? "found" : "NOT found", mPonChannel));

  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_NOTIFY,
                VirtualAddressChange,
                NULL,
                &gEfiEventVirtualAddressChangeGuid,
                &mVirtualAddressChangeEvent
                );
}
