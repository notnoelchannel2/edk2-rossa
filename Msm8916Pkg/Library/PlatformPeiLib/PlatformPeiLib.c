/** @file
 *
 *  Copyright (c) 2011-2014, ARM Limited. All rights reserved.
 *  Copyright (c) 2014, Linaro Limited. All rights reserved.
 *
 *  This program and the accompanying materials
 *  are licensed and made available under the terms and conditions of the BSD
 *License which accompanies this distribution.  The full text of the license may
 *be found at http://opensource.org/licenses/bsd-license.php
 *
 *  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR
 *IMPLIED.
 *
 **/

#include <PiPei.h>

#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>

#define MOTOR_GPIO_NONE   0xFFFFFFFF
#define TLMM_CFG(n)       (FixedPcdGet64(PcdGpioTlmmBaseAddress) + \
                           FixedPcdGet64(PcdGpioTlmmCtlOffset) + \
                           (n) * FixedPcdGet64(PcdGpioTlmmCtlElementSize))
#define TLMM_IN_OUT(n)    (FixedPcdGet64(PcdGpioTlmmBaseAddress) + \
                           FixedPcdGet64(PcdGpioTlmmIoOffset) + \
                           (n) * FixedPcdGet64(PcdGpioTlmmIoElementSize))
#define TLMM_CFG_OE       (1U << 9)
#define TLMM_CFG_DRV_2MA  (0U << 6)
#define TLMM_OUT_HIGH     (1U << 1)

STATIC VOID
TlmmOutput (
  IN UINT32   Gpio,
  IN BOOLEAN  High
  )
{
  /* Set the level first so the pin doesn't glitch when the output turns on */
  MmioWrite32(TLMM_IN_OUT(Gpio), High ? TLMM_OUT_HIGH : 0);
  /* GPIO function 0, no pull, 2 mA, output enable */
  MmioWrite32(TLMM_CFG(Gpio), TLMM_CFG_OE | TLMM_CFG_DRV_2MA);
}

/*
 * Vibration motor with a PWM input (Galaxy A3 2015): park the haptic driver
 * with the enable line low and the PWM line at a constant 100% duty, so
 * switching PcdMotorEnableGpio alone turns the motor on and off, for the
 * firmware's diagnostics and for OS drivers (rossakeys.sys) that only know
 * a single motor GPIO.
 */
STATIC VOID
MotorInit (
  VOID
  )
{
  UINT32 Enable = FixedPcdGet32(PcdMotorEnableGpio);
  UINT32 Pwm    = FixedPcdGet32(PcdMotorPwmGpio);

  if (Pwm == MOTOR_GPIO_NONE) {
    return;
  }

  if (Enable != MOTOR_GPIO_NONE) {
    TlmmOutput(Enable, FALSE);
  }
  TlmmOutput(Pwm, TRUE);
}

EFI_STATUS
EFIAPI
PlatformPeim(VOID)
{

  BuildFvHob(PcdGet64(PcdFvBaseAddress), PcdGet32(PcdFvSize));

  MotorInit();

  return EFI_SUCCESS;
}