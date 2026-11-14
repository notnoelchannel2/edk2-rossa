/*
 * Bring up the MSM8916 ChipIdea USB 2.0 controller (usb@78d9000) in host
 * mode and leave it for the OS. Windows only has the generic EHCI driver
 * (ACPI PNP0D20, see USB0 in the DSDT), which knows nothing about the
 * Qualcomm clocks, resets or the ULPI PHY, so all of that happens here.
 *
 * Sequence follows Linux drivers/usb/chipidea/ci_hdrc_msm.c and
 * drivers/phy/qualcomm/phy-qcom-usb-hs.c. The PHY tuning values are the
 * msm8916 "qcom,init-seq" from msm8916.dtsi.
 *
 * VBUS: on the J5 the 5V for OTG devices comes from the boost converter in
 * the SM5703 charger (I2C 0x49 on a GPIO bus), not from the PMIC or PHY.
 * EnableOtgVbus() does what Samsung's kernel does when an OTG cable shows up
 * (drivers/leds/sm5703_fled.c sm5703_boost_notification), but only when no
 * external VBUS (PC, charger) is present.
 *
 * rossa (PcdOtgBoostRt5033): the boost is in the RT5033 charger (I2C 0x34 on
 * GPIO 22/23) and the MUIC is an SM5504 (same registers as the SM5703 MUIC).
 * The boost is only turned on when the MUIC sees an OTG adapter (ID pin
 * grounded, ADC 0x00) at boot and no charger/PC is powering the phone.
 */

#include <PiDxe.h>
#include <Library/LKEnvLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/GpioI2cLib.h>
#include <Library/QcomClockLib.h>

#define GPIO_NONE                  0xFFFFFFFF

/* PM8916 LDOs feeding the USB HS PHY: SPMI SID 1, peripherals 0x46 / 0x4C */
#define PM8916_LDO7                0x14600 /* 1.8V */
#define PM8916_LDO13               0x14C00 /* 3.075V */
#define LDO_STATUS1                0x08
#define   LDO_STATUS1_READY        0x80
#define LDO_VSET                   0x41
#define LDO_EN_CTL                 0x46
#define   LDO_EN_CTL_ENABLE        0x80

/*
 * Minimal SPMI PMIC arbiter v2 access with timeouts (Linux spmi-pmic-arb.c).
 * The tree's SpmiDxe polls without a timeout and never completes here.
 */
#define SPMI_CORE                  0x0200F000
#define SPMI_CORE_SIZE             0x1000
#define SPMI_CHNLS                 0x02400000
#define SPMI_OBSRVR                0x02C00000
#define SPMI_CNFG                  0x0200A000
#define SPMI_EE                    0 /* apps */
#define SPMI_APID_MAP(n)           (SPMI_CORE + 0x800 + 4 * (n))
#define SPMI_APID_OWNER(n)         (SPMI_CNFG + 0x700 + 4 * (n))
#define SPMI_CMD                   0x00
#define SPMI_STATUS                0x08
#define SPMI_WDATA0                0x10
#define SPMI_RDATA0                0x18
#define SPMI_OP_EXT_WRITEL         0
#define SPMI_OP_EXT_READL          1
#define SPMI_NO_APID               0xFFFF

#define SM5703_STATUS1             0x08
#define   SM5703_STATUS1_OTGFAIL   0x80
#define SM5703_CNTL                0x0C
#define   SM5703_OPMODE_MASK       0x07
#define   SM5703_OPMODE_CHARGING   0x05
#define   SM5703_OPMODE_USB_OTG    0x07
#define SM5703_FLEDCNTL6           0x19
#define   SM5703_BSTOUT_MASK       0x0F
#define   SM5703_BSTOUT_5P0        0x0A
#define SM5703_OTGCURRENTCNTL      0x60
#define   SM5703_OTGCURRENT_MASK   0x03
#define   SM5703_OTGCURRENT_0P9A   0x02
#define SM5703_STATUS5             0x6B
#define   SM5703_STATUS5_VBUSOK    0x20

/* SM5703 MUIC (separate I2C device), see Samsung sm5703-muic.c sm5703_muic_set_otg() */
#define MUIC_DEVICE_ID             0x01
#define MUIC_CONTROL               0x02
/*
 * Samsung init writes SWITCH_OPEN (bit4, 1 = switches operate normally) |
 * RAW_DATA | MANUAL_SW | WAIT = 0x1E, OTG then clears MANUAL_SW (bit2,
 * cleared = manual switching). Bootloader "rustproof" mode may have left the
 * switches forced open, so write the whole register.
 */
#define   MUIC_CONTROL_OTG         0x1A
#define MUIC_DEVICE_TYPE1          0x0A
#define MUIC_DEVICE_TYPE2          0x0B
#define MUIC_DEVICE_TYPE3          0x15
#define MUIC_ADC                   0x07
#define MUIC_MANUAL_SW1            0x13
#define   MUIC_SW1_USB             0x25 /* D+/D- to the SoC USB PHY */
#define MUIC_MANUAL_SW2            0x14
#define   MUIC_SW2_OTG             0x02 /* disconnect MUIC_ID and ITBP */

#define GCC_USB_HS_BCR             0x01841000
#define GCC_USB2A_PHY_BCR          0x01841028
#define GCC_USB2A_PHY_SLEEP_CBCR   0x0184102C

/* Step logging at ERROR level so a hang shows the last step on screen */
#define STEP(s) DEBUG((EFI_D_ERROR, "UsbHost: " s "\n"))

#define USB_BASE                   0x078D9000
#define USB_ID                     (USB_BASE + 0x000)
#define USB_SBUSCFG                (USB_BASE + 0x090)
#define USB_AHB_MODE               (USB_BASE + 0x098)
#define USB_GENCONFIG              (USB_BASE + 0x09C)
#define   GENCONFIG_TXFIFO_IDLE_FORCE_DIS  BIT4
#define USB_GENCONFIG_2            (USB_BASE + 0x0A0)
#define   GENCONFIG_2_ULPI_TX_PKT_EN_CLR_FIX BIT19
#define   GENCONFIG_2_SESS_VLD_CTRL_EN       BIT7
#define USB_CAP                    (USB_BASE + 0x100)
#define USB_HCCPARAMS              (USB_CAP + 0x08)
#define   HCCPARAMS_LEN            BIT17
#define USB_HS_PHY_CTRL            (USB_BASE + 0x240)
#define   HS_PHY_POR_ASSERT        BIT0
/* From Qualcomm's downstream phy-msm-usb.c (28nm integrated PHY) */
#define   HS_PHY_RETEN             BIT1  /* active low: 0 = PHY in retention */
#define   HS_PHY_CLAMP_DPDMSE_EN   BIT21 /* clamps D+/D- while suspended */
#define USB_HS_PHY_CTRL2           (USB_BASE + 0x278)

/* Operational registers, relative to CAP + CAPLENGTH */
#define OP_USBCMD                  0x00
#define   USBCMD_RS                BIT0
#define   USBCMD_RST               BIT1
#define   USBCMD_SESS_VLD_CTRL     BIT25 /* software session valid (with GENCONFIG_2 bit 7) */
#define OP_ULPI_VIEWPORT           0x30
#define   ULPI_WAKEUP              BIT31
#define   ULPI_RUN                 BIT30
#define   ULPI_WRITE               BIT29
#define OP_PORTSC                  0x44
#define   PORTSC_PP                BIT12
#define   PORTSC_PTS_MASK          (0x3U << 30 | BIT25)
#define   PORTSC_PTS_ULPI          (0x2U << 30)
#define OP_DEVLC                   0x84
#define   DEVLC_PTS_MASK           (0x7U << 29)
#define   DEVLC_PTS_ULPI           (0x2U << 29)
#define OP_USBMODE_NOLPM           0x68
#define OP_USBMODE_LPM             0xC8
#define   USBMODE_CM_MASK          0x3
#define   USBMODE_CM_HOST          0x3

/* ULPI registers */
#define ULPI_FUNC_CTRL             0x04
#define ULPI_FUNC_CTRL_SET         0x05
#define ULPI_FUNC_CTRL_CLR         0x06
#define   ULPI_FUNC_TERMSELECT     BIT2
#define   ULPI_FUNC_XCVR_FS        0x01
#define   ULPI_FUNC_SUSPENDM       BIT6
#define ULPI_IFC_CTRL              0x07
#define ULPI_OTG_CTRL              0x0A
#define ULPI_USB_INT_STS           0x13
#define ULPI_DEBUG                 0x15 /* bits 1:0 = LineState (D+/D-) */
#define ULPI_OTG_CTRL_SET          0x0B
#define   ULPI_OTG_DP_PULLDOWN     BIT1
#define   ULPI_OTG_DM_PULLDOWN     BIT2
#define ULPI_EXT_VENDOR_SPECIFIC   0x80
/* Qualcomm vendor ULPI registers (downstream msm_hsusb_hw.h / phy-msm-usb.c) */
#define ULPI_CHG_DET_CTRL          0x84 /* BC1.2 charger detection, SET +1, CLR +2 */
#define ULPI_CHG_DET_CLR           0x86
#define   ULPI_CHG_DET_ALL         0x3F
#define ULPI_MISC_A                0x96
#define ULPI_MISC_A_SET            0x97
#define   ULPI_MISC_A_VBUSVLDEXT   BIT0
#define   ULPI_MISC_A_VBUSVLDEXTSEL BIT1

STATIC UINTN mOp;
STATIC BOOLEAN mExternalVbus;

STATIC BOOLEAN
WaitClear (
  IN UINTN   Reg,
  IN UINT32  Mask,
  IN UINTN   TimeoutUs
  )
{
  while (MmioRead32(Reg) & Mask) {
    if (TimeoutUs-- == 0) {
      return FALSE;
    }
    MicroSecondDelay(1);
  }
  return TRUE;
}

STATIC EFI_STATUS
UlpiWrite (
  IN UINT8  Reg,
  IN UINT8  Val
  )
{
  UINTN Viewport = mOp + OP_ULPI_VIEWPORT;

  /* Make sure the PHY is awake before talking to it */
  MmioWrite32(Viewport, ULPI_WAKEUP);
  if (!WaitClear(Viewport, ULPI_WAKEUP, 10000)) {
    return EFI_TIMEOUT;
  }

  MmioWrite32(Viewport, ULPI_RUN | ULPI_WRITE | ((UINT32)Reg << 16) | Val);
  if (!WaitClear(Viewport, ULPI_RUN, 10000)) {
    return EFI_TIMEOUT;
  }

  return EFI_SUCCESS;
}

STATIC UINT8
UlpiRead (
  IN UINT8  Reg
  )
{
  UINTN Viewport = mOp + OP_ULPI_VIEWPORT;

  MmioWrite32(Viewport, ULPI_WAKEUP);
  if (!WaitClear(Viewport, ULPI_WAKEUP, 10000)) {
    return 0xFF;
  }

  MmioWrite32(Viewport, ULPI_RUN | ((UINT32)Reg << 16));
  if (!WaitClear(Viewport, ULPI_RUN, 10000)) {
    return 0xFF;
  }

  return (UINT8)(MmioRead32(Viewport) >> 8);
}

STATIC UINT16
SpmiFindApid (
  IN UINT32  Addr
  )
{
  UINT16 Ppid = (UINT16)(Addr >> 8) & 0xFFF; /* SID << 8 | peripheral */
  UINT16 Apid;
  UINT32 Val;

  for (Apid = 0; 0x800 + 4 * Apid < SPMI_CORE_SIZE; Apid++) {
    Val = MmioRead32(SPMI_APID_MAP(Apid));
    if (Val != 0 && ((Val >> 8) & 0xFFF) == Ppid) {
      return Apid;
    }
  }

  return SPMI_NO_APID;
}

STATIC EFI_STATUS
SpmiWaitDone (
  IN UINTN  Chan
  )
{
  UINT32 Status;
  UINTN  Timeout;

  for (Timeout = 1000; Timeout > 0; Timeout--) {
    Status = MmioRead32(Chan + SPMI_STATUS);
    if (Status & BIT0) {
      if (Status & BIT2) {
        return EFI_ACCESS_DENIED;
      }
      return (Status & (BIT1 | BIT3)) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
    }
    MicroSecondDelay(1);
  }

  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
SpmiRead (
  IN  UINT32  Addr,
  OUT UINT8   *Val
  )
{
  UINT16     Apid = SpmiFindApid(Addr);
  UINTN      Chan;
  EFI_STATUS Status;

  if (Apid == SPMI_NO_APID) {
    return EFI_NOT_FOUND;
  }

  Chan = SPMI_OBSRVR + 0x1000 * SPMI_EE + 0x8000 * Apid;
  MmioWrite32(Chan + SPMI_CMD, (SPMI_OP_EXT_READL << 27) | ((Addr & 0xFF) << 4));
  Status = SpmiWaitDone(Chan);
  if (!EFI_ERROR(Status)) {
    *Val = (UINT8)MmioRead32(Chan + SPMI_RDATA0);
  }
  return Status;
}

STATIC EFI_STATUS
SpmiWrite (
  IN UINT32  Addr,
  IN UINT8   Val
  )
{
  UINT16 Apid = SpmiFindApid(Addr);
  UINTN  Chan;

  if (Apid == SPMI_NO_APID) {
    return EFI_NOT_FOUND;
  }

  Chan = SPMI_CHNLS + 0x1000 * SPMI_EE + 0x8000 * Apid;
  MmioWrite32(Chan + SPMI_WDATA0, Val);
  MmioWrite32(Chan + SPMI_CMD, (SPMI_OP_EXT_WRITEL << 27) | ((Addr & 0xFF) << 4));
  return SpmiWaitDone(Chan);
}

/*
 * The PHY's analog side (VBUS comparators, D+/D- line receivers) runs from
 * LDO13. If it is off ULPI still works, but the PHY sees no VBUS and SE0.
 * Only switch it on ourselves if the apps EE owns the regulator; otherwise
 * it belongs to the RPM and must be voted for through it.
 */
STATIC VOID
EnsurePhySupply (
  IN CONST CHAR8  *Name,
  IN UINT32       Ldo
  )
{
  UINT16     Apid = SpmiFindApid(Ldo);
  UINT8      Owner;
  UINT8      St   = 0;
  UINT8      En   = 0;
  UINT8      Vset = 0;
  EFI_STATUS Status;

  if (Apid == SPMI_NO_APID) {
    DEBUG((EFI_D_ERROR, "UsbHost: %a not in SPMI channel map\n", Name));
    return;
  }

  Owner  = (UINT8)(MmioRead32(SPMI_APID_OWNER(Apid)) & 0x7);
  Status = SpmiRead(Ldo + LDO_STATUS1, &St);
  if (!EFI_ERROR(Status)) {
    Status = SpmiRead(Ldo + LDO_EN_CTL, &En);
  }
  if (!EFI_ERROR(Status)) {
    Status = SpmiRead(Ldo + LDO_VSET, &Vset);
  }
  DEBUG((EFI_D_ERROR, "UsbHost: %a apid %u owner EE%u: status %02x en_ctl %02x vset %02x (%r)\n",
         Name, Apid, Owner, St, En, Vset, Status));
  if (EFI_ERROR(Status) || (St & LDO_STATUS1_READY)) {
    return;
  }

  if (Owner != SPMI_EE) {
    DEBUG((EFI_D_ERROR, "UsbHost: %a is off and owned by EE%u (RPM), cannot switch it on directly\n",
          Name, Owner));
    return;
  }

  Status = SpmiWrite(Ldo + LDO_EN_CTL, En | LDO_EN_CTL_ENABLE);
  MicroSecondDelay(1000);
  SpmiRead(Ldo + LDO_STATUS1, &St);
  DEBUG((EFI_D_ERROR, "UsbHost: %a enable: %r, status now %02x\n", Name, Status, St));
}

STATIC VOID
BlockReset (
  IN UINTN  Bcr
  )
{
  MmioWrite32(Bcr, 1);
  MicroSecondDelay(1000);
  MmioWrite32(Bcr, 0);
  MicroSecondDelay(1000);
}

STATIC EFI_STATUS
I2cReadReg (
  IN  GPIO_I2C_BUS  *Bus,
  IN  UINT8         Addr,
  IN  UINT8         Reg,
  OUT UINT8         *Val
  )
{
  return GpioI2cTransfer(Bus, Addr, &Reg, 1, Val, 1);
}

STATIC EFI_STATUS
I2cUpdateReg (
  IN GPIO_I2C_BUS  *Bus,
  IN UINT8         Addr,
  IN UINT8         Reg,
  IN UINT8         Mask,
  IN UINT8         Val
  )
{
  EFI_STATUS Status;
  UINT8      Buf[2];

  Status = I2cReadReg(Bus, Addr, Reg, &Buf[1]);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  Buf[0] = Reg;
  Buf[1] = (Buf[1] & ~Mask) | (Val & Mask);
  return GpioI2cTransfer(Bus, Addr, Buf, 2, NULL, 0);
}

/*
 * In auto mode the MUIC does not connect the connector's D+/D- to the SoC
 * for an OTG adapter; Samsung's kernel switches it to manual USB routing.
 */
STATIC VOID
DumpMuic (
  IN GPIO_I2C_BUS  *Bus,
  IN UINT8         Addr,
  IN CONST CHAR8   *When
  )
{
  STATIC CONST UINT8 Regs[] = {
    MUIC_DEVICE_ID, MUIC_CONTROL, MUIC_ADC, MUIC_DEVICE_TYPE1,
    MUIC_DEVICE_TYPE2, MUIC_DEVICE_TYPE3, MUIC_MANUAL_SW1, MUIC_MANUAL_SW2
  };
  UINT8 Val[ARRAY_SIZE(Regs)];
  UINTN Index;

  for (Index = 0; Index < ARRAY_SIZE(Regs); Index++) {
    if (EFI_ERROR(I2cReadReg(Bus, Addr, Regs[Index], &Val[Index]))) {
      Val[Index] = 0xEE;
    }
  }

  DEBUG((EFI_D_ERROR, "UsbHost: MUIC %a: id %02x ctrl %02x adc %02x dev %02x/%02x/%02x sw1 %02x sw2 %02x\n",
         When, Val[0], Val[1], Val[2], Val[3], Val[4], Val[5], Val[6], Val[7]));
}

/*
 * In auto mode the MUIC does not connect the connector's D+/D- to the SoC
 * for an OTG adapter; Samsung's kernel switches it to manual USB routing
 * (sm5703-muic.c sm5703_muic_reg_init + sm5703_muic_set_otg).
 */
STATIC VOID
RouteMuicToUsb (
  VOID
  )
{
  GPIO_I2C_BUS Bus;
  UINT8        Addr = FixedPcdGet8(PcdMuicI2cAddress);
  UINT8        Buf[2];
  EFI_STATUS   Status;

  if (FixedPcdGet32(PcdMuicI2cSdaGpio) == GPIO_NONE) {
    return;
  }

  Bus.SdaGpio      = FixedPcdGet32(PcdMuicI2cSdaGpio);
  Bus.SclGpio      = FixedPcdGet32(PcdMuicI2cSclGpio);
  Bus.HalfPeriodUs = 5;
  GpioI2cInit(&Bus);

  DumpMuic(&Bus, Addr, "before");

  Buf[0] = MUIC_MANUAL_SW1;
  Buf[1] = MUIC_SW1_USB;
  Status = GpioI2cTransfer(&Bus, Addr, Buf, 2, NULL, 0);
  if (!EFI_ERROR(Status)) {
    Buf[0] = MUIC_MANUAL_SW2;
    Buf[1] = MUIC_SW2_OTG;
    Status = GpioI2cTransfer(&Bus, Addr, Buf, 2, NULL, 0);
  }
  if (!EFI_ERROR(Status)) {
    Buf[0] = MUIC_CONTROL;
    Buf[1] = MUIC_CONTROL_OTG;
    Status = GpioI2cTransfer(&Bus, Addr, Buf, 2, NULL, 0);
  }
  MicroSecondDelay(10000);

  DumpMuic(&Bus, Addr, "after ");
  DEBUG((EFI_D_ERROR, "UsbHost: MUIC routed USB lines to SoC: %r\n", Status));
}

/* RT5033: Linux rt5033_charger.c rt5033_charger_set_otg() */
#define RT5033_CHG_STAT            0x00
#define   RT5033_CHG_STAT_MASK     0x30   /* 0 = discharging: no external power */
#define RT5033_CHG_CTRL1           0x01
#define   RT5033_CTRL1_MODE_BOOST  0x01
#define   RT5033_CTRL1_HZ          0x02
#define RT5033_CHG_CTRL2           0x02
#define   RT5033_CTRL2_CV_MASK     0xFC
#define   RT5033_CTRL2_BOOST_5V    (0x37 << 2)
#define MUIC_ADC_OTG               0x00   /* ID pin grounded */

STATIC BOOLEAN
MuicSeesOtgAdapter (
  VOID
  )
{
  GPIO_I2C_BUS Bus;
  UINT8        Adc;

  if (FixedPcdGet32(PcdMuicI2cSdaGpio) == GPIO_NONE) {
    return FALSE;
  }
  Bus.SdaGpio      = FixedPcdGet32(PcdMuicI2cSdaGpio);
  Bus.SclGpio      = FixedPcdGet32(PcdMuicI2cSclGpio);
  Bus.HalfPeriodUs = 5;
  GpioI2cInit(&Bus);
  if (EFI_ERROR(I2cReadReg(&Bus, FixedPcdGet8(PcdMuicI2cAddress), MUIC_ADC, &Adc))) {
    DEBUG((EFI_D_ERROR, "UsbHost: MUIC not responding\n"));
    return FALSE;
  }
  DEBUG((EFI_D_ERROR, "UsbHost: MUIC ADC 0x%02x (%a)\n", Adc, (Adc & 0x1F) == MUIC_ADC_OTG ? "OTG adapter" : "no OTG adapter"));
  return (Adc & 0x1F) == MUIC_ADC_OTG;
}

STATIC VOID
EnableOtgVbusRt5033 (
  VOID
  )
{
  GPIO_I2C_BUS Bus;
  UINT8        Addr = FixedPcdGet8(PcdOtgBoostI2cAddress);
  UINT8        Stat;
  EFI_STATUS   Status;

  Bus.SdaGpio      = FixedPcdGet32(PcdOtgBoostI2cSdaGpio);
  Bus.SclGpio      = FixedPcdGet32(PcdOtgBoostI2cSclGpio);
  Bus.HalfPeriodUs = 5;
  GpioI2cInit(&Bus);

  Status = I2cReadReg(&Bus, Addr, RT5033_CHG_STAT, &Stat);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "UsbHost: RT5033 not responding: %r\n", Status));
    return;
  }
  if (Stat & RT5033_CHG_STAT_MASK) {
    DEBUG((EFI_D_ERROR, "UsbHost: external power present (stat %02x), OTG power stays off\n", Stat));
    mExternalVbus = TRUE;
    return;
  }
  if (!MuicSeesOtgAdapter()) {
    DEBUG((EFI_D_ERROR, "UsbHost: plug the OTG adapter in before booting to get USB host power\n"));
    return;
  }

  Status = I2cUpdateReg(&Bus, Addr, RT5033_CHG_CTRL1, RT5033_CTRL1_HZ, 0);
  if (!EFI_ERROR(Status)) {
    Status = I2cUpdateReg(&Bus, Addr, RT5033_CHG_CTRL2, RT5033_CTRL2_CV_MASK, RT5033_CTRL2_BOOST_5V);
  }
  if (!EFI_ERROR(Status)) {
    Status = I2cUpdateReg(&Bus, Addr, RT5033_CHG_CTRL1, RT5033_CTRL1_MODE_BOOST, RT5033_CTRL1_MODE_BOOST);
  }
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "UsbHost: enabling RT5033 OTG boost failed: %r\n", Status));
    return;
  }
  MicroSecondDelay(100000);
  DEBUG((EFI_D_ERROR, "UsbHost: OTG power on (RT5033 boost, 5V)\n"));

  RouteMuicToUsb();
}

STATIC VOID
EnableOtgVbus (
  VOID
  )
{
  GPIO_I2C_BUS Bus;
  UINT8        Status5;
  UINT8        Status1;
  EFI_STATUS   Status;

  if (FixedPcdGet32(PcdOtgBoostI2cSdaGpio) == GPIO_NONE) {
    return;
  }
  if (FixedPcdGetBool(PcdOtgBoostRt5033)) {
    EnableOtgVbusRt5033();
    return;
  }

  Bus.SdaGpio      = FixedPcdGet32(PcdOtgBoostI2cSdaGpio);
  Bus.SclGpio      = FixedPcdGet32(PcdOtgBoostI2cSclGpio);
  Bus.HalfPeriodUs = 5;
  GpioI2cInit(&Bus);

  Status = I2cReadReg(&Bus, FixedPcdGet8(PcdOtgBoostI2cAddress), SM5703_STATUS5, &Status5);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "UsbHost: charger not responding: %r\n", Status));
    return;
  }

  if (Status5 & SM5703_STATUS5_VBUSOK) {
    /* Never fight a PC or charger that is already driving VBUS */
    DEBUG((EFI_D_ERROR, "UsbHost: external VBUS present, OTG power stays off\n"));
    mExternalVbus = TRUE;
    return;
  }

  Status = I2cUpdateReg(&Bus, FixedPcdGet8(PcdOtgBoostI2cAddress), SM5703_OTGCURRENTCNTL, SM5703_OTGCURRENT_MASK, SM5703_OTGCURRENT_0P9A);
  if (!EFI_ERROR(Status)) {
    Status = I2cUpdateReg(&Bus, FixedPcdGet8(PcdOtgBoostI2cAddress), SM5703_FLEDCNTL6, SM5703_BSTOUT_MASK, SM5703_BSTOUT_5P0);
  }
  if (!EFI_ERROR(Status)) {
    Status = I2cUpdateReg(&Bus, FixedPcdGet8(PcdOtgBoostI2cAddress), SM5703_CNTL, SM5703_OPMODE_MASK, SM5703_OPMODE_USB_OTG);
  }
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "UsbHost: enabling OTG power failed: %r\n", Status));
    return;
  }

  /* Give the boost time to come up and attached devices time to power on */
  MicroSecondDelay(100000);

  if (!EFI_ERROR(I2cReadReg(&Bus, FixedPcdGet8(PcdOtgBoostI2cAddress), SM5703_STATUS1, &Status1)) &&
      (Status1 & SM5703_STATUS1_OTGFAIL)) {
    DEBUG((EFI_D_ERROR, "UsbHost: OTG boost overcurrent, turning it off\n"));
    I2cUpdateReg(&Bus, FixedPcdGet8(PcdOtgBoostI2cAddress), SM5703_CNTL, SM5703_OPMODE_MASK, SM5703_OPMODE_CHARGING);
    return;
  }

  DEBUG((EFI_D_ERROR, "UsbHost: OTG power on (5.0V, 0.9A limit)\n"));

  RouteMuicToUsb();
}

STATIC VOID
DumpPort (
  IN CONST CHAR8  *When,
  IN BOOLEAN      Lpm
  )
{
  UINT32 PortSc = MmioRead32(mOp + OP_PORTSC);
  UINT32 Mode   = MmioRead32(mOp + (Lpm ? OP_USBMODE_LPM : OP_USBMODE_NOLPM));
  UINT32 OtgSc  = MmioRead32(mOp + (Lpm ? 0xC4 : 0x64));

  UINT8  IntSts = UlpiRead(ULPI_USB_INT_STS);

  DEBUG((EFI_D_ERROR, "UsbTest [%a]\n", When));
  DEBUG((EFI_D_ERROR, "  CMD %08x STS %08x CM %u | PORTSC %08x connect %u en %u pwr %u PTS %u\n",
         MmioRead32(mOp + OP_USBCMD), MmioRead32(mOp + 0x04), Mode & 3,
         PortSc, PortSc & 1, (PortSc >> 2) & 1, (PortSc >> 12) & 1, PortSc >> 30));
  DEBUG((EFI_D_ERROR, "  OTGSC %08x GENCFG2 %08x | ULPI func %02x ifc %02x otg %02x misc_a %02x\n",
         OtgSc, MmioRead32(USB_GENCONFIG_2), UlpiRead(ULPI_FUNC_CTRL), UlpiRead(ULPI_IFC_CTRL),
         UlpiRead(ULPI_OTG_CTRL), UlpiRead(ULPI_MISC_A)));
  DEBUG((EFI_D_ERROR, "  PHY sees: VBUS valid %u, session valid %u, ID grounded %u, LineState %u (0=SE0 none, 1/2=device)\n",
         (IntSts >> 1) & 1, (IntSts >> 2) & 1, (IntSts >> 4) & 1, UlpiRead(ULPI_DEBUG) & 3));
}

STATIC EFI_STATUS
ResetAndConfigure (
  IN BOOLEAN  Lpm
  );

/*
 * Diagnostic build only (PcdUsbHostSelfTest): show whether the PHY and the
 * port see the plugged in device with the controller halted and running,
 * then do the HCRESET Windows' usbehci does, redo the PHY init like Linux
 * does after every ChipIdea reset, and look again.
 */
STATIC VOID
TryRouting (
  IN CONST CHAR8  *Name,
  IN BOOLEAN      Lpm
  )
{
  UINT32 PortSc;
  UINTN  Ms;

  /* Re-assert port power and give the device a second to attach */
  MmioOr32(mOp + OP_PORTSC, PORTSC_PP);
  for (Ms = 0; Ms < 1000 && !(MmioRead32(mOp + OP_PORTSC) & BIT0); Ms++) {
    MicroSecondDelay(1000);
  }

  PortSc = MmioRead32(mOp + OP_PORTSC);
  DEBUG((EFI_D_ERROR, "Try %a: connect %u LS %u PORTSC %08x ULPI linestate %u func %02x\n",
         Name, PortSc & 1, (PortSc >> 10) & 3, PortSc, UlpiRead(ULPI_DEBUG) & 3,
         UlpiRead(ULPI_FUNC_CTRL)));
}

/*
 * Diagnostic build only (PcdUsbHostSelfTest): with the controller running
 * in host mode, try the MUIC routings that could connect the connector's
 * D+/D- to the PHY and report whether the port sees the device for each.
 */
STATIC VOID
ProbeDevice (
  IN CONST CHAR8  *Name,
  IN BOOLEAN      Lpm
  )
{
  UINT32 Sts;
  UINT32 PortSc;

  MmioAnd32(mOp + OP_USBCMD, ~USBCMD_RS);
  MicroSecondDelay(10000);
  MmioOr32(mOp + OP_USBCMD, USBCMD_RST);
  WaitClear(mOp + OP_USBCMD, USBCMD_RST, 100000);

  if (Lpm) {
    MmioAndThenOr32(mOp + OP_DEVLC, ~DEVLC_PTS_MASK, DEVLC_PTS_ULPI);
  } else {
    MmioAndThenOr32(mOp + OP_PORTSC, ~PORTSC_PTS_MASK, PORTSC_PTS_ULPI);
  }
  UlpiWrite(ULPI_MISC_A_SET, ULPI_MISC_A_VBUSVLDEXTSEL | ULPI_MISC_A_VBUSVLDEXT);
  MmioOr32(USB_GENCONFIG_2, GENCONFIG_2_SESS_VLD_CTRL_EN);
  MmioOr32(mOp + OP_USBCMD, USBCMD_SESS_VLD_CTRL);
  MmioWrite32(USB_SBUSCFG, 0);
  MmioWrite32(USB_AHB_MODE, 0x8);
  MmioAndThenOr32(mOp + (Lpm ? OP_USBMODE_LPM : OP_USBMODE_NOLPM), ~USBMODE_CM_MASK, 0x2);

  MmioWrite32(mOp + 0x04, 0xFFFFFFFF);
  MmioOr32(mOp + OP_USBCMD, USBCMD_RS | USBCMD_SESS_VLD_CTRL);
  MicroSecondDelay(6000000);

  Sts    = MmioRead32(mOp + 0x04);
  PortSc = MmioRead32(mOp + OP_PORTSC);
  DEBUG((EFI_D_ERROR, "Probe %a: bus reset %u PORTSC %08x en %u speed %u func %02x ls %u\n",
         Name, (Sts >> 6) & 1, PortSc, (PortSc >> 2) & 1, (PortSc >> 26) & 3,
         UlpiRead(ULPI_FUNC_CTRL), UlpiRead(ULPI_DEBUG) & 3));

  /* Drop off the bus so the PC sees a detach between probes */
  MmioAnd32(mOp + OP_USBCMD, ~USBCMD_RS);
  MicroSecondDelay(500000);
}

/*
 * Diagnostic build only, PC on the cable: starting from the state lk2nd's
 * working fastboot USB left behind, add our init differences one at a time
 * and see after which one the PC stops seeing the phone.
 */
STATIC VOID
DeviceProbeMatrix (
  VOID
  )
{
  struct clk *Iface = gClock->clk_get("usb_iface_clk");
  struct clk *Core  = gClock->clk_get("usb_core_clk");
  BOOLEAN    Lpm;

  /* Enable only, no rate change: keep whatever lk2nd programmed */
  gClock->clk_enable(Iface);
  gClock->clk_enable(Core);

  mOp = USB_CAP + (MmioRead32(USB_CAP) & 0xFF);
  Lpm = (MmioRead32(USB_HCCPARAMS) & HCCPARAMS_LEN) != 0;
  DEBUG((EFI_D_ERROR, "Probe: start PORTSC %08x USBMODE %08x sleep_cbcr %08x\n",
         MmioRead32(mOp + OP_PORTSC), MmioRead32(mOp + (Lpm ? OP_USBMODE_LPM : OP_USBMODE_NOLPM)),
         MmioRead32(GCC_USB2A_PHY_SLEEP_CBCR)));

  ProbeDevice("A lk2nd state, no GCC changes", Lpm);

  MmioOr32(GCC_USB2A_PHY_SLEEP_CBCR, BIT0);
  ProbeDevice("B + PHY sleep clock on", Lpm);

  BlockReset(GCC_USB_HS_BCR);
  ProbeDevice("C + USB_HS_BCR, clocks on", Lpm);

  /* lk2nd hsusb_clock_init(): clocks off, block reset, clocks on */
  gClock->clk_disable(Core);
  gClock->clk_disable(Iface);
  MicroSecondDelay(20000);
  MmioWrite32(GCC_USB_HS_BCR, 1);
  MicroSecondDelay(20000);
  MmioWrite32(GCC_USB_HS_BCR, 0);
  MicroSecondDelay(20000);
  gClock->clk_enable(Iface);
  gClock->clk_enable(Core);
  ProbeDevice("D lk2nd order: clk off, BCR, clk on", Lpm);

  DEBUG((EFI_D_ERROR, "UsbTest: done, holding 60s for a photo\n"));
  MicroSecondDelay(60 * 1000000);
}

STATIC VOID
SelfTest (
  IN BOOLEAN  Lpm
  )
{

  DumpPort("host mode, as set up", Lpm);
  MmioOr32(mOp + OP_USBCMD, USBCMD_RS | USBCMD_SESS_VLD_CTRL);

  TryRouting("1 as set up", Lpm);

  /*
   * FUNC_CTRL reads 0x45 in host mode: TermSelect=1 is the device-side
   * 1.5k D+ pull-up. A host needs TermSelect=0 (only the 15k pull-downs).
   */
  UlpiWrite(ULPI_FUNC_CTRL_CLR, ULPI_FUNC_TERMSELECT);
  TryRouting("2 TermSelect cleared", Lpm);

  UlpiWrite(ULPI_FUNC_CTRL, ULPI_FUNC_SUSPENDM | ULPI_FUNC_XCVR_FS);
  UlpiWrite(ULPI_OTG_CTRL_SET, ULPI_OTG_DP_PULLDOWN | ULPI_OTG_DM_PULLDOWN);
  TryRouting("3 FUNC=41 + pulldowns", Lpm);

  MmioAnd32(mOp + OP_PORTSC, ~PORTSC_PP);
  MicroSecondDelay(200000);
  UlpiWrite(ULPI_FUNC_CTRL, ULPI_FUNC_SUSPENDM | ULPI_FUNC_XCVR_FS);
  TryRouting("4 port power cycled", Lpm);

  /* Controller halted: does the link still own FUNC_CTRL? */
  MmioAnd32(mOp + OP_USBCMD, ~USBCMD_RS);
  MicroSecondDelay(10000);
  UlpiWrite(ULPI_FUNC_CTRL, ULPI_FUNC_SUSPENDM | ULPI_FUNC_XCVR_FS);
  TryRouting("5 halted, FUNC=41", Lpm);
  MmioOr32(mOp + OP_USBCMD, USBCMD_RS | USBCMD_SESS_VLD_CTRL);

  DumpPort("end", Lpm);
  MmioAnd32(mOp + OP_USBCMD, ~USBCMD_RS);

  DEBUG((EFI_D_ERROR, "UsbTest: done, holding 60s for a photo\n"));
  MicroSecondDelay(60 * 1000000);
}

/*
 * Controller reset, ULPI PHY init and host mode. ChipIdea also resets the
 * ULPI PHY on a controller reset, so this whole sequence belongs together
 * (Linux redoes it on every CI_HDRC_CONTROLLER_RESET_EVENT).
 */
STATIC EFI_STATUS
ResetAndConfigure (
  IN BOOLEAN  Lpm
  )
{
  EFI_STATUS Status;

  STEP("controller reset");
  MmioOr32(mOp + OP_USBCMD, USBCMD_RST);
  if (!WaitClear(mOp + OP_USBCMD, USBCMD_RST, 100000)) {
    DEBUG((EFI_D_ERROR, "UsbHost: controller reset timed out\n"));
    return EFI_DEVICE_ERROR;
  }

  STEP("ULPI PHY init");
  /* ULPI PHY interface */
  if (Lpm) {
    MmioAndThenOr32(mOp + OP_DEVLC, ~DEVLC_PTS_MASK, DEVLC_PTS_ULPI);
  } else {
    MmioAndThenOr32(mOp + OP_PORTSC, ~PORTSC_PTS_MASK, PORTSC_PTS_ULPI);
  }

  /*
   * On the J5 the PHY's VBUS pin is not wired to the connector (VBUS is
   * sensed by the MUIC/charger), so the PHY and controller never see a valid
   * session by themselves. lk2nd's working fastboot USB forces both in
   * software (target_usb_init: ULPI MISC_A, GENCONFIG_2 bit 7, USBCMD bit 25),
   * as does Linux ci_hdrc_msm when VBUS comes from an extcon. We supply VBUS
   * from the SM5703 boost, so claiming it is valid is the truth.
   */
  Status = UlpiWrite(ULPI_MISC_A_SET, ULPI_MISC_A_VBUSVLDEXTSEL | ULPI_MISC_A_VBUSVLDEXT);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "UsbHost: ULPI write failed: %r (PHY unpowered?)\n", Status));
    return Status;
  }
  MmioOr32(USB_GENCONFIG_2, GENCONFIG_2_SESS_VLD_CTRL_EN);
  MmioOr32(mOp + OP_USBCMD, USBCMD_SESS_VLD_CTRL);

  STEP("host mode");
  /* Same bus setup as lk2nd: AHB burst off, HPROT mode */
  MmioWrite32(USB_SBUSCFG, 0);
  MmioWrite32(USB_AHB_MODE, 0x8);

  /* Host instead of lk2nd's device mode, D+/D- pulldowns, port power */
  MmioAndThenOr32(mOp + (Lpm ? OP_USBMODE_LPM : OP_USBMODE_NOLPM), ~USBMODE_CM_MASK, USBMODE_CM_HOST);
  Status = UlpiWrite(ULPI_OTG_CTRL_SET, ULPI_OTG_DP_PULLDOWN | ULPI_OTG_DM_PULLDOWN);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "UsbHost: setting D+/D- pulldowns failed: %r\n", Status));
  }
  MmioOr32(mOp + OP_PORTSC, PORTSC_PP);

  DEBUG((EFI_D_ERROR, "UsbHost: PHY_CTRL %08x, ULPI tuning %02x %02x %02x %02x\n",
         MmioRead32(USB_HS_PHY_CTRL), UlpiRead(0x80), UlpiRead(0x81), UlpiRead(0x82), UlpiRead(0x83)));

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
UsbHostDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  BOOLEAN           Lpm;
  EFI_STATUS        Status;
  INTN              Ret;

  STEP("OTG power");
  EnableOtgVbus();

  STEP("PHY supplies");
  DEBUG((EFI_D_ERROR, "UsbHost: SPMI arbiter version %08x\n", MmioRead32(SPMI_CORE)));
  EnsurePhySupply("LDO7", PM8916_LDO7);
  EnsurePhySupply("LDO13", PM8916_LDO13);

  if (FixedPcdGetBool(PcdUsbHostSelfTest) && mExternalVbus) {
    DeviceProbeMatrix();
  }

  STEP("clocks");
  Ret = gClock->clk_get_set_enable("usb_iface_clk", 0, 1);
  if (Ret == 0) {
    Ret = gClock->clk_get_set_enable("usb_core_clk", 80000000, 1);
  }
  if (Ret != 0) {
    DEBUG((EFI_D_ERROR, "UsbHost: failed to enable clocks (%d)\n", Ret));
    return EFI_DEVICE_ERROR;
  }
  MmioOr32(GCC_USB2A_PHY_SLEEP_CBCR, BIT0);

  STEP("block resets");
  /* Start from a clean controller and PHY */
  BlockReset(GCC_USB_HS_BCR);

  mOp = USB_CAP + (MmioRead32(USB_CAP) & 0xFF);
  Lpm = (MmioRead32(USB_HCCPARAMS) & HCCPARAMS_LEN) != 0;

  Status = ResetAndConfigure(Lpm);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  DEBUG((EFI_D_INFO, "UsbHost: ID 0x%08x, LPM %u, PORTSC 0x%08x, host mode ready\n",
         MmioRead32(USB_ID), Lpm, MmioRead32(mOp + OP_PORTSC)));

  if (FixedPcdGetBool(PcdUsbHostSelfTest)) {
    SelfTest(Lpm);
  }

  return EFI_SUCCESS;
}
