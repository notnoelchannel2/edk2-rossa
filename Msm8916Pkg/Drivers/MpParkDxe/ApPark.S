/*
 * Parking loop for CPU1-3 (ACPI Multiprocessor Parking Protocol v1).
 * Copied to MPPARK_BASE (0x80300000) and entered by TrustZone at cold boot,
 * in AArch64, MMU and caches off. Position independent.
 *
 * Mailbox for CPU n: MPPARK_BASE + 0x1000 * (n + 1); live mask at +0x5000
 *   +0x000 ProcessorId (UINT32)   OS writes the CPU's ACPI UID to release it
 *   +0x008 JumpAddress (UINT64)   where to go; zeroed by us as acknowledgment
 *   +0x800 firmware area: we write 'PARK' there once alive
 *
 * WFE is woken by the generic timer event stream (~every 0.1 ms), so the
 * cores need no GIC setup and the OS needs no SEV.
 *
 * The first word is valid in both instruction sets: in AArch64 it is a
 * harmless TST, in AArch32 a branch to Park32. Some TrustZone builds start
 * secondaries in AArch32 whatever we ask; Park32 then reports 'A32!' at
 * +0x804, waits for the firmware's 'GO64' at +0x810 and asks TZ (the same
 * SCM "milestone" call lk2nd uses to boot 64-bit kernels) to restart it in
 * AArch64 at ApParkStart, using the parameter block at 0x80306000+0x1000*n.
 * Markers: +0x808 'A64 ' (entered AArch64), +0x80c 'FAIL', +0x814 SCM result,
 * +0x818 step reached, +0x81c 'EXC!' + 0x840 entry EL, 0x844 fault EL,
 * 0x848 vector, 0x850 ESR, 0x858 ELR, 0x860 FAR. The vectors must sit at
 * ApParkStart + 0x800, so the blob is copied to a 2 KB aligned address.
 */
  .text
  .align 3
  .global ApParkStart
  .global ApParkEnd

ApParkStart:
  .word 0xEA000000 | (((Park32 - ApParkStart - 8) >> 2) & 0xFFFFFF)
  msr   daifset, #0xf
  mrs   x0, mpidr_el1
  and   x0, x0, #0xff              // Aff0: CPU 0-3
  adr   x1, ApParkStart
  add   x1, x1, #0x1000
  add   x1, x1, x0, lsl #12        // our mailbox
  movz  w2, #0x3641                // 'A64 ' (0x20343641)
  movk  w2, #0x2034, lsl #16
  str   w2, [x1, #0x808]
  dsb   sy

  // Catch exceptions: vectors at ApParkStart + 0x800 record what went wrong
  // and fall back to a polling park loop, so a fault never loses the core.
  mrs   x5, CurrentEL
  str   w5, [x1, #0x840]
  adr   x2, ApVectors
  cmp   x5, #(2 << 2)
  b.ne  1f
  msr   vbar_el2, x2
1:
  msr   vbar_el1, x2
  isb
  mov   w2, #1                     // step 1: vectors set
  str   w2, [x1, #0x818]
  dsb   sy

  cmp   x5, #(2 << 2)
  b.ne  2f
  mov   w2, #2                     // step 2: CNTHCTL_EL2 (at EL2 only)
  str   w2, [x1, #0x818]
  dsb   sy
  mrs   x3, cnthctl_el2
  bic   x3, x3, #0xf0
  mov   x4, #((10 << 4) | (1 << 2) | 3)
  orr   x3, x3, x4
  msr   cnthctl_el2, x3
2:
  mov   w2, #3                     // step 3: CNTKCTL_EL1 event stream
  str   w2, [x1, #0x818]
  dsb   sy
  mov   x3, #((10 << 4) | (1 << 2))  // EVNTI = 10, EVNTEN
  msr   cntkctl_el1, x3
  isb

  mov   w2, #4                     // step 4: parked, WFE loop
  str   w2, [x1, #0x818]
  movz  w2, #0x524b                // PARK (0x5041524B)
  movk  w2, #0x5041, lsl #16
  str   w2, [x1, #0x800]
  dsb   sy

ParkWfe:
  wfe
  ldr   w3, [x1]                   // ProcessorId
  cmp   w3, w0
  b.ne  ParkWfe
  ldr   x4, [x1, #8]               // JumpAddress
  cbz   x4, ParkWfe
  b     Release

ParkPoll:                          // no WFE: used after a fault
  mov   x5, #2000
3:
  subs  x5, x5, #1
  b.ne  3b
  ldr   w3, [x1]
  cmp   w3, w0
  b.ne  ParkPoll
  ldr   x4, [x1, #8]
  cbz   x4, ParkPoll

Release:
  str   xzr, [x1, #8]              // acknowledge
  dsb   sy
  mov   x0, x1                     // mailbox address, for the OS's benefit
  br    x4

ApFault:                           // x9 = vector index
  mrs   x5, CurrentEL
  str   w5, [x1, #0x844]
  str   x9, [x1, #0x848]
  cmp   x5, #(2 << 2)
  b.eq  4f
  mrs   x6, esr_el1
  mrs   x7, elr_el1
  mrs   x8, far_el1
  b     5f
4:
  mrs   x6, esr_el2
  mrs   x7, elr_el2
  mrs   x8, far_el2
5:
  str   x6, [x1, #0x850]
  str   x7, [x1, #0x858]
  str   x8, [x1, #0x860]
  movz  w2, #0x5845                // 'EXC!' (0x21435845)
  movk  w2, #0x2143, lsl #16
  str   w2, [x1, #0x81c]
  movz  w2, #0x524b                // still usable: PARK, polling loop
  movk  w2, #0x5041, lsl #16
  str   w2, [x1, #0x800]
  dsb   sy
  b     ParkPoll

  .align 3
Park32:                            // AArch32: ApPark32.s, assembled with
                                   // llvm-mc --triple=armv7a -mattr=+trustzone
  // 132 bytes
  .word 0xf10c01c0
  .word 0xee108fb0
  .word 0xe20880ff
  .word 0xe3017000
  .word 0xe3487030
  .word 0xe0877608
  .word 0xe3032341
  .word 0xe3422132
  .word 0xe5872804
  .word 0xf57ff04f
  .word 0xe3043f47
  .word 0xe3433436
  .word 0xe5972810
  .word 0xe1520003
  .word 0x1afffffc
  .word 0xe300010f
  .word 0xe3400200
  .word 0xe3a01012
  .word 0xe3062000
  .word 0xe3482030
  .word 0xe0822608
  .word 0xe3a03050
  .word 0xe3a04000
  .word 0xe3a05000
  .word 0xe3a06000
  .word 0xe1600070
  .word 0xe5870814
  .word 0xe3042146
  .word 0xe3442c49
  .word 0xe587280c
  .word 0xf57ff04f
  .word 0xe320f002
  .word 0xeafffffd

  .org ApParkStart + 0x800          // VBAR needs 2 KB alignment: blob goes to 0x80300000
ApVectors:
  .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
  .org ApVectors + 128 * \n
  mov   x9, #\n
  b     ApFault
  .endr

  .org ApParkStart + 0x1000
ApParkEnd:
