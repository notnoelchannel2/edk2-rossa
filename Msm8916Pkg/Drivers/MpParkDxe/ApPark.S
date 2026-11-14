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
 */
  .text
  .align 3
  .global ApParkStart
  .global ApParkEnd

ApParkStart:
  msr   daifset, #0xf
  mrs   x0, mpidr_el1
  and   x0, x0, #0xff              // Aff0: CPU 0-3
  adr   x1, ApParkStart
  add   x1, x1, #0x1000
  add   x1, x1, x0, lsl #12        // our mailbox

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
ApParkEnd:
