#include <PiDxe.h>
#include <Library/LKEnvLib.h>
#include <Library/QcomTargetMmcSdhciLib.h>
#include <Library/QcomGpioTlmmLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Chipset/mmc_sdhci.h>
#include <Platform/iomap.h>
#include <Platform/irqs.h>

#define MMC_SLOT_EMMC 1
#define MMC_SLOT_SD   2

#define GPIO_NONE 0xFFFFFFFF

static uint32_t mmc_pwrctl_base[] =
	{ MSM_SDC1_BASE, MSM_SDC2_BASE };

static uint32_t mmc_sdhci_base[] =
	{ MSM_SDC1_SDHCI_BASE, MSM_SDC2_SDHCI_BASE };

static uint32_t  mmc_sdc_pwrctl_irq[] =
	{ SDCC1_PWRCTL_IRQ, SDCC2_PWRCTL_IRQ };

STATIC VOID FillSlotConfig(struct mmc_config_data *config, uint32_t slot)
{
  config->slot        = slot;
  config->sdhc_base   = mmc_sdhci_base[slot - 1];
  config->pwrctl_base = mmc_pwrctl_base[slot - 1];
  config->pwr_irq     = mmc_sdc_pwrctl_irq[slot - 1];
}

STATIC VOID ConfigureSdc1Pins(VOID)
{
  uint32_t reg = SDC1_HDRV_PULL_CTL;

  /* Drive strength configs for sdc pins */
  struct tlmm_cfgs sdc1_hdrv_cfg[] = {
      {SDC1_CLK_HDRV_CTL_OFF, TLMM_CUR_VAL_16MA, TLMM_HDRV_MASK, reg},
      {SDC1_CMD_HDRV_CTL_OFF, TLMM_CUR_VAL_10MA, TLMM_HDRV_MASK, reg},
      {SDC1_DATA_HDRV_CTL_OFF, TLMM_CUR_VAL_10MA, TLMM_HDRV_MASK, reg},
  };

  /* Pull configs for sdc pins */
  struct tlmm_cfgs sdc1_pull_cfg[] = {
      {SDC1_CLK_PULL_CTL_OFF, TLMM_NO_PULL, TLMM_PULL_MASK, reg},
      {SDC1_CMD_PULL_CTL_OFF, TLMM_PULL_UP, TLMM_PULL_MASK, reg},
      {SDC1_DATA_PULL_CTL_OFF, TLMM_PULL_UP, TLMM_PULL_MASK, reg},
  };

  struct tlmm_cfgs sdc1_rclk_cfg[] = {
      {SDC1_RCLK_PULL_CTL_OFF, TLMM_PULL_DOWN, TLMM_PULL_MASK, reg},
  };

  gGpioTlmm->tlmm_set_hdrive_ctrl(sdc1_hdrv_cfg, ARRAY_SIZE(sdc1_hdrv_cfg));
  gGpioTlmm->tlmm_set_pull_ctrl(sdc1_pull_cfg, ARRAY_SIZE(sdc1_pull_cfg));
  gGpioTlmm->tlmm_set_pull_ctrl(sdc1_rclk_cfg, ARRAY_SIZE(sdc1_rclk_cfg));
}

STATIC VOID ConfigureSdc2Pins(VOID)
{
  uint32_t reg = SDC2_HDRV_PULL_CTL;

  /* SDC2 has its own bit layout (clk pull is at bit 14, not 13) */
  struct tlmm_cfgs sdc2_hdrv_cfg[] = {
      {SDC2_CLK_HDRV_CTL_OFF, TLMM_CUR_VAL_16MA, TLMM_HDRV_MASK, reg},
      {SDC2_CMD_HDRV_CTL_OFF, TLMM_CUR_VAL_10MA, TLMM_HDRV_MASK, reg},
      {SDC2_DATA_HDRV_CTL_OFF, TLMM_CUR_VAL_10MA, TLMM_HDRV_MASK, reg},
  };

  struct tlmm_cfgs sdc2_pull_cfg[] = {
      {SDC2_CLK_PULL_CTL_OFF, TLMM_NO_PULL, TLMM_PULL_MASK, reg},
      {SDC2_CMD_PULL_CTL_OFF, TLMM_PULL_UP, TLMM_PULL_MASK, reg},
      {SDC2_DATA_PULL_CTL_OFF, TLMM_PULL_UP, TLMM_PULL_MASK, reg},
  };

  gGpioTlmm->tlmm_set_hdrive_ctrl(sdc2_hdrv_cfg, ARRAY_SIZE(sdc2_hdrv_cfg));
  gGpioTlmm->tlmm_set_pull_ctrl(sdc2_pull_cfg, ARRAY_SIZE(sdc2_pull_cfg));
}

STATIC BOOLEAN SdCardPresent(VOID)
{
  UINT32  Gpio      = FixedPcdGet32(PcdSdCardDetectGpio);
  BOOLEAN ActiveLow = FixedPcdGetBool(PcdSdCardDetectActiveLow);
  UINTN   Level;

  if (Gpio == GPIO_NONE) {
    /* No card detect line, just try to initialize the slot */
    return TRUE;
  }

  gGpioTlmm->SetFunction(Gpio, 0);
  gGpioTlmm->SetPull(Gpio, GPIO_PULL_NONE);
  gGpioTlmm->SetDriveStrength(Gpio, 2);
  gGpioTlmm->DirectionInput(Gpio);
  MicroSecondDelay(100);

  Level = gGpioTlmm->Get(Gpio);
  DEBUG((EFI_D_INFO, "SD card detect gpio%u = %u\n", Gpio, Level));

  return ActiveLow ? (Level == 0) : (Level != 0);
}

STATIC VOID InitEmmc(INIT_SLOT_CB InitSlot)
{
  struct mmc_config_data config = {0};

  DEBUG((EFI_D_LOAD, "Initializing eMMC (mmc slot %u)\n", MMC_SLOT_EMMC));

  ConfigureSdc1Pins();

  FillSlotConfig(&config, MMC_SLOT_EMMC);
  config.bus_width     = DATA_BUS_WIDTH_8BIT;
  config.max_clk_rate  = MMC_CLK_177MHZ;
  config.hs200_support = 1;
  config.hs400_support = 0;

  /* InitSlot steps down HS200 -> HS52 -> 25MHz by itself if reads fail */
  if (InitSlot(&config) == NULL) {
    DEBUG((EFI_D_ERROR, "Can't initialize eMMC (mmc slot %u)\n", MMC_SLOT_EMMC));
  }
}

STATIC VOID InitSdCard(INIT_SLOT_CB InitSlot)
{
  struct mmc_config_data config = {0};

  if (!SdCardPresent()) {
    DEBUG((EFI_D_INFO, "No SD card inserted, skipping mmc slot %u\n", MMC_SLOT_SD));
    return;
  }

  DEBUG((EFI_D_LOAD, "Initializing SD card (mmc slot %u)\n", MMC_SLOT_SD));

  ConfigureSdc2Pins();

  /* SD cards are at most 4 bit wide; no UHS-I so the SDHCI divider caps at 50MHz */
  FillSlotConfig(&config, MMC_SLOT_SD);
  config.bus_width     = DATA_BUS_WIDTH_4BIT;
  config.max_clk_rate  = MMC_CLK_200MHZ;
  config.hs200_support = 0;
  config.hs400_support = 0;

  if (InitSlot(&config) == NULL) {
    DEBUG((EFI_D_ERROR, "Can't initialize SD card (mmc slot %u)\n", MMC_SLOT_SD));
  }
}

VOID LibQcomTargetMmcSdhciInit(INIT_SLOT_CB InitSlot)
{
  /* eMMC first so it always gets the first BlockIo handle */
  InitEmmc(InitSlot);
  InitSdCard(InitSlot);
}
