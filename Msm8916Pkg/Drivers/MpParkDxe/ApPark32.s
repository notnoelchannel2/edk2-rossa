@ AArch32 half of the parking code (not built by edk2: its words are in ApPark.S)
@ llvm-mc --triple=armv7a-none-eabi -mattr=+trustzone -filetype=obj ApPark32.s -o a.o
@ llvm-objcopy -O binary -j .text a.o a.bin
  .syntax unified
  .arm
a32_entry:
  cpsid aif
  mrc   p15, 0, r8, c0, c0, 5      @ MPIDR
  and   r8, r8, #0xff              @ CPU number
  movw  r7, #0x1000
  movt  r7, #0x8030                @ 0x80301000
  add   r7, r7, r8, lsl #12        @ our mailbox
  movw  r2, #0x3341
  movt  r2, #0x2132                @ 'A32!'
  str   r2, [r7, #0x804]
  dsb   sy
  movw  r3, #0x4f47
  movt  r3, #0x3436                @ 'GO64'
1:
  ldr   r2, [r7, #0x810]
  cmp   r2, r3
  bne   1b
  movw  r0, #0x010f
  movt  r0, #0x0200                @ SIP: milestone 32->64 (elexec)
  mov   r1, #0x12                  @ 2 args, arg0 = buffer (read)
  movw  r2, #0x6000
  movt  r2, #0x8030
  add   r2, r2, r8, lsl #12        @ param block for this CPU
  mov   r3, #80
  mov   r4, #0
  mov   r5, #0
  mov   r6, #0
  smc   #0
  str   r0, [r7, #0x814]           @ only reached if the switch failed
  movw  r2, #0x4146
  movt  r2, #0x4c49                @ 'FAIL'
  str   r2, [r7, #0x80c]
  dsb   sy
2:
  wfe
  b     2b
