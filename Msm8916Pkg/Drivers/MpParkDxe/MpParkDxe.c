/*
 * MpParkDxe - bring up CPU1-3 on MSM8916 and park them for Windows
 * (ACPI Multiprocessor Parking Protocol v1).
 *
 * Samsung's TrustZone on rossa has no PSCI, so we do what lk2nd's
 * spin-table does (lk2nd/smp/cpu-boot.c, cortex-a.c, BSD-3-Clause):
 *   1. copy the parking loop (ApPark.S) to MPPARK_BASE (reserved memory)
 *   2. SCM BOOT_SET_ADDR_MC: cold-boot address for all CPUs, AArch64
 *   3. power each core up through its ACC (APCS power controller)
 *   4. wait for the core to write 'PARK' to its mailbox
 *
 * Only the cores that arrive are published: at ReadyToBoot the MADT is
 * replaced by one with a GICC (and parking mailbox) per live core, and the
 * DSDT's CPU devices read the live-core mask at MPPARK_BASE + 0xF00. If
 * anything fails, Windows simply keeps seeing one core (as before).
 */
#include <Uefi.h>
#include <IndustryStandard/Acpi.h>
#include <Guid/EventGroup.h>
#include <Library/ArmLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/AcpiSystemDescriptionTable.h>
#include <Protocol/AcpiTable.h>

#define MPPARK_BASE          0x80300000UL
#define MPPARK_MBOX(n)       (MPPARK_BASE + 0x1000 * ((n) + 1))
#define MPPARK_CPU_MASK      (MPPARK_BASE + 0xF00)   /* read by the DSDT */
#define MPPARK_MAGIC         0x5041524BU             /* 'PARK' */
#define MBOX_PROCESSOR_ID    0x000
#define MBOX_JUMP_ADDRESS    0x008
#define MBOX_FW_ALIVE        0x800

#define CPU_COUNT            4

/* ACC (per-CPU power controller) */
STATIC CONST UINTN mAccBase[CPU_COUNT] = { 0x0B088000, 0x0B098000, 0x0B0A8000, 0x0B0B8000 };
#define CPU_PWR_CTL                 0x04
#define APC_PWR_GATE_CTL            0x14
#define CPU_PWR_CTL_CLAMP           BIT0
#define CPU_PWR_CTL_CORE_MEM_CLAMP  BIT1
#define CPU_PWR_CTL_CORE_MEM_HS     BIT3
#define CPU_PWR_CTL_CORE_RST        BIT4
#define CPU_PWR_CTL_COREPOR_RST     BIT5
#define CPU_PWR_CTL_CORE_PWRD_UP    BIT7
#define APC_PWR_GATE_CTL_GHDS_EN    BIT0
#define APC_PWR_GATE_CTL_GHDS_CNT(c) ((c) << 24)

/* SCM (SMC calling convention, SMC32 like lk2nd) */
#define SCM_SIP(svc, cmd)           (0x02000000U | (((svc) << 8) | (cmd)))
#define SCM_SVC_BOOT                0x01
#define SCM_SVC_INFO                0x06
#define SCM_IS_CALL_AVAIL           0x01
#define SCM_BOOT_SET_ADDR_MC        0x11
#define SCM_BOOT_MC_FLAG_AARCH64    BIT0
#define SCM_BOOT_MC_FLAG_COLDBOOT   BIT1
#define SCM_INTERRUPTED             1

extern UINT8 ApParkStart[];
extern UINT8 ApParkEnd[];

STATIC UINT32    mLiveMask = BIT0;   /* CPU0 is us */
STATIC EFI_EVENT mReadyToBootEvent;

STATIC
UINTN
ScmCall (
  IN OUT ARM_SMC_ARGS  *Args
  )
{
  ARM_SMC_ARGS Saved = *Args;
  UINTN        Try;

  for (Try = 0; Try < 10; Try++) {
    *Args = Saved;
    ArmCallSmc(Args);
    if ((UINT32)Args->Arg0 != SCM_INTERRUPTED) {
      break;
    }
  }
  return (UINT32)Args->Arg0;
}

STATIC
BOOLEAN
ScmBootAddrAvailable (
  VOID
  )
{
  ARM_SMC_ARGS Args;

  ZeroMem(&Args, sizeof(Args));
  Args.Arg0 = SCM_SIP(SCM_SVC_INFO, SCM_IS_CALL_AVAIL);
  Args.Arg1 = 1;  /* one value argument */
  Args.Arg2 = SCM_SIP(SCM_SVC_BOOT, SCM_BOOT_SET_ADDR_MC);
  if (ScmCall(&Args) != 0) {
    DEBUG((EFI_D_WARN, "MpPark: SCM IS_CALL_AVAIL failed (0x%x): no ARMv8 SCM\n", (UINT32)Args.Arg0));
    return FALSE;
  }
  return (UINT32)Args.Arg1 != 0;
}

STATIC
EFI_STATUS
ScmSetColdBootAddr (
  IN UINTN  Addr
  )
{
  ARM_SMC_ARGS Args;
  UINT32       *Indirect;
  UINTN        Ret;

  /* Args 3..5 go in a buffer whose address is passed in x5 */
  Indirect = AllocateAlignedPages(1, 64);
  if (Indirect == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  ZeroMem(Indirect, EFI_PAGE_SIZE);
  Indirect[0] = 0xFFFFFFFF;                                              /* aff2: all */
  Indirect[1] = 0xFFFFFFFF;                                              /* aff3: all */
  Indirect[2] = SCM_BOOT_MC_FLAG_AARCH64 | SCM_BOOT_MC_FLAG_COLDBOOT;    /* flags */
  WriteBackInvalidateDataCacheRange(Indirect, EFI_PAGE_SIZE);

  ZeroMem(&Args, sizeof(Args));
  Args.Arg0 = SCM_SIP(SCM_SVC_BOOT, SCM_BOOT_SET_ADDR_MC);
  Args.Arg1 = 6;                 /* six value arguments */
  Args.Arg2 = (UINT32)Addr;
  Args.Arg3 = 0xFFFFFFFF;        /* aff0: all */
  Args.Arg4 = 0xFFFFFFFF;        /* aff1: all */
  Args.Arg5 = (UINT32)(UINTN)Indirect;
  Ret = ScmCall(&Args);

  /* TZ has read it by now; keep the page if the call failed oddly */
  if (Ret == 0) {
    FreeAlignedPages(Indirect, 1);
    return EFI_SUCCESS;
  }
  DEBUG((EFI_D_WARN, "MpPark: SCM BOOT_SET_ADDR_MC failed: 0x%x\n", (UINT32)Ret));
  return EFI_DEVICE_ERROR;
}

/* Cortex-A53 power-up through the ACC, as in lk2nd cpu_boot_cortex_a() */
STATIC
VOID
PowerUpCore (
  IN UINTN  Acc
  )
{
  UINT32 Pwr;

  Pwr = CPU_PWR_CTL_CLAMP | CPU_PWR_CTL_CORE_MEM_CLAMP | CPU_PWR_CTL_CORE_RST | CPU_PWR_CTL_COREPOR_RST;
  MmioWrite32(Acc + CPU_PWR_CTL, Pwr);
  ArmDataSynchronizationBarrier();
  MmioWrite32(Acc + APC_PWR_GATE_CTL, APC_PWR_GATE_CTL_GHDS_EN | APC_PWR_GATE_CTL_GHDS_CNT(16));
  ArmDataSynchronizationBarrier();
  MicroSecondDelay(2);

  Pwr &= ~CPU_PWR_CTL_CORE_MEM_CLAMP;
  MmioWrite32(Acc + CPU_PWR_CTL, Pwr);
  ArmDataSynchronizationBarrier();
  Pwr |= CPU_PWR_CTL_CORE_MEM_HS;
  MmioWrite32(Acc + CPU_PWR_CTL, Pwr);
  ArmDataSynchronizationBarrier();
  MicroSecondDelay(2);

  Pwr &= ~CPU_PWR_CTL_CLAMP;
  MmioWrite32(Acc + CPU_PWR_CTL, Pwr);
  ArmDataSynchronizationBarrier();
  MicroSecondDelay(2);

  Pwr &= ~(CPU_PWR_CTL_CORE_RST | CPU_PWR_CTL_COREPOR_RST);
  MmioWrite32(Acc + CPU_PWR_CTL, Pwr);
  ArmDataSynchronizationBarrier();
  Pwr |= CPU_PWR_CTL_CORE_PWRD_UP;
  MmioWrite32(Acc + CPU_PWR_CTL, Pwr);
  ArmDataSynchronizationBarrier();
}

#define MAGIC_A32            0x21323341U   /* 'A32!' at +0x804: started in AArch32 */
#define MAGIC_A64            0x20343641U   /* 'A64 ' at +0x808: entered AArch64 */
#define MAGIC_FAIL           0x4C494146U   /* 'FAIL' at +0x80C: 32->64 switch refused */
#define MAGIC_GO64           0x34364F47U   /* 'GO64' at +0x810: firmware says switch now */
#define MBOX_A32             0x804
#define MBOX_A64             0x808
#define MBOX_FAIL            0x80C
#define MBOX_GO64            0x810
#define MBOX_SCM_RESULT      0x814
#define PARAM_BLOCK(n)       (MPPARK_BASE + 0x6000 + 0x1000 * (n))   /* el1_system_param */

typedef enum {
  CoreSilent,      /* never ran our code */
  CoreA32,         /* in Park32, waiting for GO64 */
  CoreA64,         /* entered AArch64 but did not park */
  CoreParked,
  CoreSwitchFailed /* TZ refused the 32->64 switch */
} CORE_STATE;

STATIC
CORE_STATE
CoreState (
  IN UINTN  Cpu
  )
{
  UINTN Mbox = MPPARK_MBOX(Cpu);

  if (MmioRead32(Mbox + MBOX_FW_ALIVE) == MPPARK_MAGIC) {
    return CoreParked;
  }
  if (MmioRead32(Mbox + MBOX_FAIL) == MAGIC_FAIL) {
    return CoreSwitchFailed;
  }
  if (MmioRead32(Mbox + MBOX_A64) == MAGIC_A64) {
    return CoreA64;
  }
  if (MmioRead32(Mbox + MBOX_A32) == MAGIC_A32) {
    return CoreA32;
  }
  return CoreSilent;
}

/* Wait until the core leaves state From (or Ms milliseconds pass) */
STATIC
CORE_STATE
WaitChange (
  IN UINTN       Cpu,
  IN CORE_STATE  From,
  IN UINTN       Ms
  )
{
  CORE_STATE State = From;
  UINTN      Us;

  for (Us = 0; Us < Ms * 1000; Us += 100) {
    State = CoreState(Cpu);
    if (State != From && State != CoreA64) {
      return State;
    }
    MicroSecondDelay(100);
  }
  return CoreState(Cpu);
}

/* Old SCM convention, as the Samsung 32-bit kernel uses: cold boot in AArch32 */
STATIC
UINTN
ScmLegacySetBootAddr (
  IN UINTN  Addr
  )
{
  ARM_SMC_ARGS  Args;
  STATIC UINT32 Context;

  ZeroMem(&Args, sizeof(Args));
  Args.Arg0 = 0x00401222;            /* SCM_ATOMIC(SVC_BOOT, SET_ADDR, 2 args) */
  Args.Arg1 = (UINTN)&Context;
  Args.Arg2 = BIT0 | BIT3 | BIT5;    /* cold boot CPU1, CPU2, CPU3 */
  Args.Arg3 = Addr;
  ArmCallSmc(&Args);
  return (UINT32)Args.Arg0;
}

STATIC
VOID
BringUpCores (
  VOID
  )
{
  STATIC CONST CHAR8 *Names[] = { "never ran our code", "started in AArch32", "entered AArch64 but did not park",
                                  "parked", "TZ refused the AArch32->AArch64 switch" };
  UINTN       Cpu;
  UINTN       Size = (UINTN)(ApParkEnd - ApParkStart);
  BOOLEAN     LegacyTried = FALSE;
  UINTN       LegacyRet;
  BOOLEAN     McOk;
  CORE_STATE  State;

  /* Region is mapped uncached; still clean so the cores see it with MMU off */
  SetMem((VOID *)MPPARK_BASE, 0xA000, 0);
  for (Cpu = 0; Cpu < CPU_COUNT; Cpu++) {
    MmioWrite32(MPPARK_MBOX(Cpu) + MBOX_PROCESSOR_ID, 0xFFFFFFFF);
    /* el1_system_param: x0-x8 = 0, el1_elr (offset 72) = AArch64 entry */
    MmioWrite64(PARAM_BLOCK(Cpu) + 72, MPPARK_BASE);
  }
  CopyMem((VOID *)MPPARK_BASE, ApParkStart, Size);
  MmioWrite32(MPPARK_CPU_MASK, mLiveMask);
  WriteBackInvalidateDataCacheRange((VOID *)MPPARK_BASE, 0xA000);
  InvalidateInstructionCacheRange((VOID *)MPPARK_BASE, Size);

  McOk = ScmBootAddrAvailable();
  DEBUG((EFI_D_WARN, "MpPark: ARMv8 SCM BOOT_SET_ADDR_MC %a\n", McOk ? "available" : "NOT available"));
  if (McOk) {
    McOk = !EFI_ERROR(ScmSetColdBootAddr(MPPARK_BASE));
    DEBUG((EFI_D_WARN, "MpPark: AArch64 cold-boot address %a\n", McOk ? "set" : "refused"));
  }
  if (!McOk) {
    DEBUG((EFI_D_WARN, "MpPark: trying the legacy SCM call (AArch32 cold boot)...\n"));
    LegacyRet = ScmLegacySetBootAddr(MPPARK_BASE);
    DEBUG((EFI_D_WARN, "MpPark: legacy SET_ADDR returned 0x%x\n", LegacyRet));
    LegacyTried = TRUE;
  }

  for (Cpu = 1; Cpu < CPU_COUNT; Cpu++) {
    UINTN Acc = mAccBase[Cpu];

    if (MmioRead32(Acc + CPU_PWR_CTL) & CPU_PWR_CTL_CORE_PWRD_UP) {
      DEBUG((EFI_D_WARN, "MpPark: CPU%u already powered (by the bootloader?), skipping\n", Cpu));
      continue;
    }
    PowerUpCore(Acc);
    State = WaitChange(Cpu, CoreSilent, 100);

    if (State == CoreA32) {
      DEBUG((EFI_D_WARN, "MpPark: CPU%u started in AArch32, asking TZ to switch it...\n", Cpu));
      MmioWrite32(MPPARK_MBOX(Cpu) + MBOX_GO64, MAGIC_GO64);
      ArmDataSynchronizationBarrier();
      State = WaitChange(Cpu, CoreA32, 200);
    }

    DEBUG((EFI_D_WARN, "MpPark: CPU%u %a (PWR_CTL 0x%x, marks %x %x %x, scm 0x%x)\n", Cpu, Names[State],
           MmioRead32(Acc + CPU_PWR_CTL), MmioRead32(MPPARK_MBOX(Cpu) + MBOX_A32),
           MmioRead32(MPPARK_MBOX(Cpu) + MBOX_A64), MmioRead32(MPPARK_MBOX(Cpu) + MBOX_FW_ALIVE),
           MmioRead32(MPPARK_MBOX(Cpu) + MBOX_SCM_RESULT)));

    if (State == CoreParked) {
      mLiveMask |= 1U << Cpu;
    } else if (State == CoreSilent && !LegacyTried) {
      /* TZ ignored the AArch64 request outright: try the 32-bit kernel's way on the next core */
      DEBUG((EFI_D_WARN, "MpPark: trying the legacy SCM call (AArch32 cold boot) for the next core...\n"));
      LegacyRet = ScmLegacySetBootAddr(MPPARK_BASE);
    DEBUG((EFI_D_WARN, "MpPark: legacy SET_ADDR returned 0x%x\n", LegacyRet));
      LegacyTried = TRUE;
    }
  }
  MmioWrite32(MPPARK_CPU_MASK, mLiveMask);
}

/* --- MADT: one GICC per live core ----------------------------------------- */

#pragma pack(1)
typedef struct {
  UINT8  Type;
  UINT8  Length;
  UINT16 Reserved;
  UINT32 CpuInterfaceNumber;
  UINT32 AcpiProcessorUid;
  UINT32 Flags;
  UINT32 ParkingProtocolVersion;
  UINT32 PerformanceInterruptGsiv;
  UINT64 ParkedAddress;
  /* ACPI 5.1+ fields follow when Length > 40 */
} GICC_COMMON;
#pragma pack()

STATIC
VOID
UpdateMadt (
  VOID
  )
{
  EFI_ACPI_SDT_PROTOCOL       *Sdt;
  EFI_ACPI_TABLE_PROTOCOL     *AcpiTable;
  EFI_ACPI_SDT_HEADER         *Table;
  EFI_ACPI_TABLE_VERSION      Version;
  UINTN                       Key;
  UINTN                       Index;
  UINT8                       *Old;
  UINT8                       *New;
  UINT8                       *Src;
  UINT8                       *Dst;
  UINT8                       *GiccTemplate = NULL;
  UINT32                      OldLen;
  UINTN                       NewKey;
  UINTN                       Cpu;
  EFI_STATUS                  Status;

  if (mLiveMask == BIT0) {
    return;
  }
  if (EFI_ERROR(gBS->LocateProtocol(&gEfiAcpiSdtProtocolGuid, NULL, (VOID **)&Sdt)) ||
      EFI_ERROR(gBS->LocateProtocol(&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTable))) {
    DEBUG((EFI_D_WARN, "MpPark: no ACPI SDT/table protocol, MADT left alone\n"));
    return;
  }

  for (Index = 0; ; Index++) {
    if (EFI_ERROR(Sdt->GetAcpiTable(Index, &Table, &Version, &Key))) {
      DEBUG((EFI_D_WARN, "MpPark: no MADT found\n"));
      return;
    }
    if (Table->Signature == EFI_ACPI_1_0_APIC_SIGNATURE) {
      break;
    }
  }

  Old    = (UINT8 *)Table;
  OldLen = Table->Length;
  New    = AllocateZeroPool(OldLen + CPU_COUNT * 0x60);
  if (New == NULL) {
    return;
  }

  /* Header + fixed MADT fields; then every non-GICC entry, GICCs rebuilt */
  CopyMem(New, Old, sizeof(EFI_ACPI_DESCRIPTION_HEADER) + 8);
  Dst = New + sizeof(EFI_ACPI_DESCRIPTION_HEADER) + 8;
  for (Src = Old + sizeof(EFI_ACPI_DESCRIPTION_HEADER) + 8; Src < Old + OldLen && Src[1] != 0; Src += Src[1]) {
    if (Src[0] == EFI_ACPI_5_0_GIC) {
      if (GiccTemplate == NULL) {
        GiccTemplate = Src;
      }
      continue;
    }
  }
  if (GiccTemplate == NULL) {
    FreePool(New);
    return;
  }
  for (Cpu = 0; Cpu < CPU_COUNT; Cpu++) {
    GICC_COMMON *G = (GICC_COMMON *)Dst;
    if (!(mLiveMask & (1U << Cpu))) {
      continue;
    }
    CopyMem(Dst, GiccTemplate, GiccTemplate[1]);
    G->CpuInterfaceNumber     = (UINT32)Cpu;
    G->AcpiProcessorUid       = (UINT32)Cpu;
    G->Flags                  = 1;  /* enabled */
    G->ParkingProtocolVersion = 1;
    G->ParkedAddress          = MPPARK_MBOX(Cpu);
    if (GiccTemplate[1] >= 0x50) {
      *(UINT64 *)(Dst + 0x44) = Cpu; /* ACPI 5.1+: MPIDR */
    }
    Dst += GiccTemplate[1];
  }
  for (Src = Old + sizeof(EFI_ACPI_DESCRIPTION_HEADER) + 8; Src < Old + OldLen && Src[1] != 0; Src += Src[1]) {
    if (Src[0] != EFI_ACPI_5_0_GIC) {
      CopyMem(Dst, Src, Src[1]);
      Dst += Src[1];
    }
  }
  ((EFI_ACPI_DESCRIPTION_HEADER *)New)->Length   = (UINT32)(Dst - New);
  ((EFI_ACPI_DESCRIPTION_HEADER *)New)->Checksum = 0;
  ((EFI_ACPI_DESCRIPTION_HEADER *)New)->Checksum = CalculateCheckSum8(New, (UINTN)(Dst - New));

  Status = AcpiTable->UninstallAcpiTable(AcpiTable, Key);
  if (!EFI_ERROR(Status)) {
    Status = AcpiTable->InstallAcpiTable(AcpiTable, New, (UINTN)(Dst - New), &NewKey);
  }
  DEBUG((EFI_D_INFO, "MpPark: MADT with live cores 0x%x: %r\n", mLiveMask, Status));
  FreePool(New);
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gBS->CloseEvent(Event);  /* once is enough */
  UpdateMadt();
}

EFI_STATUS
EFIAPI
MpParkDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  BringUpCores();
  DEBUG((EFI_D_INFO, "MpPark: live CPU mask 0x%x\n", mLiveMask));

  return gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnReadyToBoot, NULL,
                            &gEfiEventReadyToBootGuid, &mReadyToBootEvent);
}
