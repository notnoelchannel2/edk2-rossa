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

#include <Protocol/AbsolutePointer.h>
#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/QcomPm8x41.h>

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

STATIC VOID
ShowTouch (
  VOID
  )
{
  EFI_ABSOLUTE_POINTER_PROTOCOL *Touch;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiAbsolutePointerProtocolGuid, NULL, (VOID **)&Touch))) {
    Print (L"Touch not found (no answer from the IST3038?)\n");
    return;
  }

  Print (L"Touch found, %lux%lu\n", Touch->Mode->AbsoluteMaxX, Touch->Mode->AbsoluteMaxY);
}

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

STATIC VOID
KeyTest (
  VOID
  )
{
  EFI_INPUT_KEY Key;
  UINTN         Index;
  BOOLEAN       HomeOnce = FALSE;

  Print (L"\nKey test: press keys. Home twice = exit.\n");
  gST->ConIn->Reset (gST->ConIn, FALSE);

  for ( ; ; ) {
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
    if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      continue;
    }

    Print (L"  %s\n", KeyName (&Key));

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

EFI_STATUS
EFIAPI
RossaDiagMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"RossaDiag - %a\n\n", (CHAR8 *)PcdGetPtr (PcdDeviceProduct));

  ShowCpu ();
  ShowPmic ();
  ShowDisplay ();
  ShowTouch ();
  ShowStorage ();

  KeyTest ();
  return EFI_SUCCESS;
}
