/*
 * Parking loop for CPU1-3 (ACPI Multiprocessor Parking Protocol v1).
 * Copied to MPPARK_BASE (0x80300000) and entered by TrustZone at cold boot,
 * in AArch64, MMU and caches off. Position independent.
 *
 * Mailbox for CPU n: MPPARK_BASE + 0x1000 * (n + 1)
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
 * Markers: +0x808 'A64 ' (entered AArch64), +0x80c 'FAIL', +0x814 SCM result.
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

  mrs   x2, CurrentEL
  cmp   x2, #(2 << 2)
  b.ne  1f
  mrs   x3, cnthctl_el2            // at EL2: event stream + EL1 counter access
  bic   x3, x3, #0xf0
  mov   x4, #((10 << 4) | (1 << 2) | 3)
  orr   x3, x3, x4
  msr   cnthctl_el2, x3
1:
  mov   x3, #((10 << 4) | (1 << 2))  // EVNTI = 10, EVNTEN
  msr   cntkctl_el1, x3
  isb

  movz  w2, #0x4b52                // 'PARK' (0x5041524B)
  movk  w2, #0x5041, lsl #16
  str   w2, [x1, #0x800]
  dsb   sy

2:
  wfe
  ldr   w3, [x1]                   // ProcessorId
  cmp   w3, w0
  b.ne  2b
  ldr   x4, [x1, #8]               // JumpAddress
  cbz   x4, 2b
  str   xzr, [x1, #8]              // acknowledge
  dsb   sy
  msr   cntkctl_el1, xzr
  isb
  mov   x0, x1                     // mailbox address, for the OS's benefit
  br    x4

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

  .align 3
ApParkEnd:
