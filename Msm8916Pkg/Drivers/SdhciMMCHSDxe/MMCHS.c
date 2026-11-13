#include "MMCHS.h"

/* Multi-block self test size: exercises ADMA and the auto CMD12/CMD23 path */
#define MMC_SELFTEST_BLOCKS  128

/*
 * Step a slot configuration down to the next, more tolerant, bus mode.
 * Returns FALSE when there is nothing slower left to try.
 *
 *   eMMC: HS200 8-bit -> HS52 SDR 8-bit -> 25MHz 4-bit -> 25MHz 1-bit
 *   SD:   HS 4-bit (50MHz) -> 25MHz 4-bit -> 25MHz 1-bit
 */
STATIC BOOLEAN
MmcDegradeConfig(struct mmc_config_data *config)
{
  if (config->hs200_support || config->hs400_support) {
    config->hs200_support = 0;
    config->hs400_support = 0;
    config->no_ddr        = 1;
    return TRUE;
  }

  if (!config->no_hs) {
    config->no_hs  = 1;
    config->no_ddr = 1;
    if (config->bus_width == DATA_BUS_WIDTH_8BIT) {
      config->bus_width = DATA_BUS_WIDTH_4BIT;
    }
    return TRUE;
  }

  if (config->bus_width != DATA_BUS_WIDTH_1BIT) {
    config->bus_width = DATA_BUS_WIDTH_1BIT;
    return TRUE;
  }

  return FALSE;
}

/* Only used in DEBUG() messages, so a macro (a function would be unused in RELEASE) */
#define MmcModeName(cfg)                                                       \
  ((cfg)->hs200_support ? "HS200" :                                            \
   (!(cfg)->no_hs ? "high speed" : "default speed (25MHz)"))

/*
 * Read the first blocks of a freshly initialized card, once as a single
 * block and once as a multi-block transfer. A bus mode that the board can't
 * sustain (signal integrity, IO voltage, marginal card) usually passes
 * identification and only fails on 4/8-bit data, so check before publishing.
 */
STATIC BOOLEAN
MmcSelfTest(struct mmc_device *dev)
{
  UINT32  Blocks = MMC_SELFTEST_BLOCKS;
  UINT32  Size;
  UINTN   Pages;
  VOID   *Base;
  VOID   *Buffer;
  BOOLEAN Ok = FALSE;

  if (dev->card.block_size == 0) {
    return FALSE;
  }
  if (dev->card.capacity / dev->card.block_size < Blocks) {
    Blocks = (UINT32)(dev->card.capacity / dev->card.block_size);
  }
  if (Blocks == 0) {
    return FALSE;
  }

  /* One guard page on each side in case a failing transfer overruns */
  Size  = Blocks * dev->card.block_size;
  Pages = EFI_SIZE_TO_PAGES(Size) + 2;
  Base  = AllocatePages(Pages);
  if (Base == NULL) {
    /* Can't test, don't hold the device back because of it */
    return TRUE;
  }
  Buffer = (UINT8 *)Base + EFI_PAGE_SIZE;

  arch_clean_invalidate_cache_range((addr_t)Buffer, Size);
  if (mmc_sdhci_read(dev, Buffer, 0, 1) == 0) {
    arch_clean_invalidate_cache_range((addr_t)Buffer, Size);
    Ok = (mmc_sdhci_read(dev, Buffer, 0, Blocks) == 0);
  }

  /* A block other than 0 too: catches block/byte addressing mix-ups (SDSC) */
  if (Ok && Blocks > 64) {
    arch_clean_invalidate_cache_range((addr_t)Buffer, Size);
    Ok = (mmc_sdhci_read(dev, Buffer, 64, 1) == 0);
  }

  /*
   * After a failed transfer the controller may still write into the buffer
   * (DMA isn't guaranteed to be stopped), so only hand it back on success.
   */
  if (Ok)
    FreePages(Base, Pages);
  return Ok;
}

/*
 * Initialize a slot, stepping the bus mode down until a real data read works.
 * The configuration that worked is kept in dev->config for runtime recovery.
 */
STATIC struct mmc_device *
MmcInitWithFallback(struct mmc_config_data *config)
{
  struct mmc_config_data cfg = *config;
  struct mmc_device     *dev;

  for (;;) {
    dev = mmc_init(&cfg);
    if (dev != NULL) {
      if (MmcSelfTest(dev)) {
        DEBUG((EFI_D_ERROR, "mmc slot %u: %a, %u-bit bus OK\n", cfg.slot,
               MmcModeName(&cfg), (cfg.bus_width == DATA_BUS_WIDTH_8BIT) ? 8 :
               (cfg.bus_width == DATA_BUS_WIDTH_4BIT) ? 4 : 1));
        return dev;
      }
      DEBUG((EFI_D_ERROR, "mmc slot %u: read test failed in %a mode\n",
             cfg.slot, MmcModeName(&cfg)));
      /*
       * Deliberately not freed: the controller's power IRQ handler and a
       * possibly still running DMA may reference it. A few KB, once.
       */
    }

    if (!MmcDegradeConfig(&cfg)) {
      DEBUG((EFI_D_ERROR, "mmc slot %u: giving up\n", cfg.slot));
      return NULL;
    }
    DEBUG((EFI_D_ERROR, "mmc slot %u: retrying in a slower mode\n", cfg.slot));
  }
}

/*
 * Runtime recovery after a failed transfer: re-initialize the card one step
 * slower and swap the device in place. Returns 0 on success.
 */
STATIC UINT32
MmcRecover(BIO_INSTANCE *Instance)
{
  struct mmc_config_data cfg = Instance->MmcDev->config;
  struct mmc_device     *dev;

  if (!MmcDegradeConfig(&cfg)) {
    return 1;
  }

  DEBUG((EFI_D_ERROR, "mmc slot %u: transfer error, re-initializing slower\n", cfg.slot));
  dev = MmcInitWithFallback(&cfg);
  if (dev == NULL) {
    return 1;
  }

  /* Must still be the same card */
  if (dev->card.capacity != Instance->MmcDev->card.capacity ||
      dev->card.block_size != Instance->MmcDev->card.block_size) {
    DEBUG((EFI_D_ERROR, "mmc slot %u: card changed during recovery\n", cfg.slot));
    return 1;
  }

  /* The old device is leaked on purpose, see MmcInitWithFallback */
  Instance->MmcDev = dev;
  return 0;
}

STATIC struct mmc_device *
PlatformCallbackInitSlot(struct mmc_config_data *config)
{
  EFI_STATUS    Status;
  BIO_INSTANCE *Instance;

  // Initialize MMC device (falls back to slower bus modes if reads fail)
  struct mmc_device *dev = MmcInitWithFallback(config);
  if (dev == NULL) {
    return NULL;
  }

  // Allocate instance
  Status = BioInstanceContructor(&Instance);
  if (EFI_ERROR(Status)) {
    return dev;
  }

  // Set data
  Instance->MmcDev               = dev;
  Instance->BlockMedia.BlockSize = dev->card.block_size;
  Instance->BlockMedia.LastBlock =
      dev->card.capacity / Instance->BlockMedia.BlockSize - 1;
  Instance->BlockMedia.RemovableMedia = (config->slot != 1);
  /* Slot 1 is the eMMC with the phone's own partitions: never write it */
  Instance->BlockMedia.ReadOnly = (config->slot == 1);

  // Give every device a slighty different GUID
  Instance->DevicePath.Mmc.Guid.Data4[7] = config->slot;

  // Slot 1 is the soldered eMMC, everything else is an SD card slot
  Instance->DevicePath.Slot.Header.SubType =
      (config->slot == 1) ? MSG_EMMC_DP : MSG_SD_DP;
  Instance->DevicePath.Slot.SlotNumber = (UINT8)(config->slot - 1);

  // Register for ExitBS event
  Status = gBS->CreateEventEx(
      EVT_NOTIFY_SIGNAL, TPL_NOTIFY, MMCHSExitBsUninit, (VOID *)Instance,
      &gEfiEventExitBootServicesGuid, &Instance->ExitBsEvent);
  ASSERT_EFI_ERROR(Status);

  // Publish BlockIO
  Status = gBS->InstallMultipleProtocolInterfaces(
      &Instance->Handle, &gEfiBlockIoProtocolGuid, &Instance->BlockIo,
      &gEfiDevicePathProtocolGuid, &Instance->DevicePath, NULL);
  ASSERT_EFI_ERROR(Status);

  return dev;
}

STATIC BIO_INSTANCE mBioTemplate = {
    BIO_INSTANCE_SIGNATURE,
    NULL, // Handle
    {
        // BlockIo
        EFI_BLOCK_IO_INTERFACE_REVISION, // Revision
        NULL,                            // *Media
        MMCHSReset,                      // Reset
        MMCHSReadBlocks,                 // ReadBlocks
        MMCHSWriteBlocks,                // WriteBlocks
        MMCHSFlushBlocks                 // FlushBlocks
    },
    {
        // BlockMedia
        BIO_INSTANCE_SIGNATURE, // MediaId
        FALSE,                  // RemovableMedia
        TRUE,                   // MediaPresent
        FALSE,                  // LogicalPartition
        FALSE,                  // ReadOnly
        FALSE,                  // WriteCaching
        0,                      // BlockSize
        4,                      // IoAlign
        0,                      // Pad
        0                       // LastBlock
    },
    {
        // DevicePath
        {
            {
                HARDWARE_DEVICE_PATH,
                HW_VENDOR_DP,
                {(UINT8)(sizeof(VENDOR_DEVICE_PATH)),
                 (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8)},
            },
            // Hardware Device Path for Bio
            EFI_CALLER_ID_GUID // Use the driver's GUID
        },
        {
            // SD/eMMC slot, SubType is fixed up per slot
            {
                MESSAGING_DEVICE_PATH,
                MSG_SD_DP,
                {(UINT8)(sizeof(SD_DEVICE_PATH)),
                 (UINT8)((sizeof(SD_DEVICE_PATH)) >> 8)},
            },
            0 // SlotNumber
        },

        {
            END_DEVICE_PATH_TYPE,
            END_ENTIRE_DEVICE_PATH_SUBTYPE,
            {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0},
        },
    },
    NULL, // MMCDev
    NULL, // ExitBS Event
};

/*
 * Function: mmc_write
 * Arg     : Data address on card, data length, i/p buffer
 * Return  : 0 on Success, non zero on failure
 * Flow    : Write the data from in to the card
 */
STATIC UINT32
mmc_write(BIO_INSTANCE *Instance, UINT64 data_addr, UINT32 data_len, VOID *in)
{
  UINT32 val        = 0;
  UINT32 block_size = 0;
  UINT32 write_size = SDHCI_ADMA_MAX_TRANS_SZ;
  UINT8 *sptr       = (UINT8 *)in;

  block_size = Instance->BlockMedia.BlockSize;

  ASSERT(!(data_addr % block_size));

  if (data_len % block_size)
    data_len = ROUNDUP(data_len, block_size);

  /*
   * Flush the cache before handing over the data to
   * storage driver
   */
  arch_clean_invalidate_cache_range((addr_t)in, data_len);

  /* TODO: This function is aware of max data that can be
   * tranferred using sdhci adma mode, need to have a cleaner
   * implementation to keep this function independent of sdhci
   * limitations
   */
  while (data_len > write_size) {
    val = mmc_sdhci_write(
        Instance->MmcDev, (VOID *)sptr, (data_addr / block_size),
        (write_size / block_size));
    if (val) {
      DEBUG(
          (EFI_D_ERROR, "Failed Writing block @ %x\n",
           (UINTN)(data_addr / block_size)));
      return val;
    }
    sptr += write_size;
    data_addr += write_size;
    data_len -= write_size;
  }

  if (data_len)
    val = mmc_sdhci_write(
        Instance->MmcDev, (VOID *)sptr, (data_addr / block_size),
        (data_len / block_size));

  if (val)
    DEBUG(
        (EFI_D_ERROR, "Failed Writing block @ %x\n",
         (UINTN)(data_addr / block_size)));

  return val;
}

/*
 * Function: mmc_read
 * Arg     : Data address on card, o/p buffer & data length
 * Return  : 0 on Success, non zero on failure
 * Flow    : Read data from the card to out
 */
STATIC UINT32
mmc_read_aligned(BIO_INSTANCE *Instance, UINT64 data_addr, UINT32 *out, UINT32 data_len);

/*
 * Function: mmc_read
 * Arg     : Data address on card, o/p buffer & data length
 * Return  : 0 on Success, non zero on failure
 * Flow    : The SDHCI DMA path needs cache line aligned read buffers (the
 *           cache invalidate after the transfer would otherwise clobber
 *           whatever shares the first/last cache line). UEFI callers only
 *           honour IoAlign, so bounce unaligned reads.
 */
STATIC UINT32
mmc_read(BIO_INSTANCE *Instance, UINT64 data_addr, UINT32 *out, UINT32 data_len)
{
  UINT32 ret;
  UINTN  pages;
  VOID  *bounce;

  if (IS_CACHE_LINE_ALIGNED(out))
    return mmc_read_aligned(Instance, data_addr, out, data_len);

  pages  = EFI_SIZE_TO_PAGES(data_len);
  bounce = AllocatePages(pages);
  if (bounce == NULL)
    return 1;

  ret = mmc_read_aligned(Instance, data_addr, bounce, data_len);
  if (ret == 0) {
    CopyMem(out, bounce, data_len);
    FreePages(bounce, pages);
  }
  /* else: leaked on purpose, a failed DMA may still land in it */
  return ret;
}

STATIC UINT32
mmc_read_chunk(BIO_INSTANCE *Instance, UINT8 *dst, UINT64 lba, UINT32 blocks)
{
  UINT32 ret;
  UINT32 size = blocks * Instance->BlockMedia.BlockSize;

  ret = mmc_sdhci_read(Instance->MmcDev, (VOID *)dst, lba, blocks);
  if (ret == 0)
    return 0;

  DEBUG((EFI_D_ERROR, "mmc slot %u: failed reading %u blocks @ 0x%lx\n",
         Instance->MmcDev->config.slot, blocks, lba));

  /* One retry in a slower bus mode */
  if (MmcRecover(Instance) == 0) {
    arch_clean_invalidate_cache_range((addr_t)dst, size);
    ret = mmc_sdhci_read(Instance->MmcDev, (VOID *)dst, lba, blocks);
    if (ret == 0)
      return 0;
    DEBUG((EFI_D_ERROR, "mmc slot %u: read @ 0x%lx still failing\n",
           Instance->MmcDev->config.slot, lba));
  }

  return ret;
}

STATIC UINT32
mmc_read_aligned(BIO_INSTANCE *Instance, UINT64 data_addr, UINT32 *out, UINT32 data_len)
{
  UINT32 ret = 0;
  UINT32 block_size;
  UINT32 read_size = SDHCI_ADMA_MAX_TRANS_SZ;
  UINT8 *sptr      = (UINT8 *)out;

  block_size = Instance->BlockMedia.BlockSize;

  ASSERT(!(data_addr % block_size));
  ASSERT(!(data_len % block_size));

  /*
   * dma onto write back memory is unsafe/nonportable,
   * but callers to this routine normally provide
   * write back buffers. Invalidate cache
   * before read data from mmc.
   */
  arch_clean_invalidate_cache_range((addr_t)(out), data_len);

  /* TODO: This function is aware of max data that can be
   * tranferred using sdhci adma mode, need to have a cleaner
   * implementation to keep this function independent of sdhci
   * limitations
   */
  while (data_len > read_size) {
    ret = mmc_read_chunk(Instance, sptr, data_addr / block_size, read_size / block_size);
    if (ret) {
      return ret;
    }
    sptr += read_size;
    data_addr += read_size;
    data_len -= read_size;
  }

  if (data_len)
    ret = mmc_read_chunk(Instance, sptr, data_addr / block_size, data_len / block_size);

  return ret;
}

EFI_STATUS
EFIAPI
MMCHSReset(IN EFI_BLOCK_IO_PROTOCOL *This, IN BOOLEAN ExtendedVerification)
{
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MMCHSReadBlocks(
    IN EFI_BLOCK_IO_PROTOCOL *This, IN UINT32 MediaId, IN EFI_LBA Lba,
    IN UINTN BufferSize, OUT VOID *Buffer)
{
  BIO_INSTANCE *      Instance;
  EFI_BLOCK_IO_MEDIA *Media;
  EFI_TPL             OldTpl;
  UINTN               BlockSize;
  UINTN               RC;

  Instance  = BIO_INSTANCE_FROM_BLOCKIO_THIS(This);
  Media     = &Instance->BlockMedia;
  BlockSize = Media->BlockSize;

  if (MediaId != Media->MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if (Lba > Media->LastBlock) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Lba + (BufferSize / BlockSize) - 1) > Media->LastBlock) {
    return EFI_INVALID_PARAMETER;
  }

  if (BufferSize % BlockSize != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }

  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  RC     = mmc_read(Instance, (UINT64)Lba * BlockSize, Buffer, BufferSize);
  gBS->RestoreTPL(OldTpl);

  if (RC == 0)
    return EFI_SUCCESS;
  else
    return EFI_DEVICE_ERROR;
}

EFI_STATUS
EFIAPI
MMCHSWriteBlocks(
    IN EFI_BLOCK_IO_PROTOCOL *This, IN UINT32 MediaId, IN EFI_LBA Lba,
    IN UINTN BufferSize, IN VOID *Buffer)
{
  BIO_INSTANCE *      Instance;
  EFI_BLOCK_IO_MEDIA *Media;
  UINTN               BlockSize;
  UINTN               RC;
  EFI_TPL             OldTpl;

  Instance  = BIO_INSTANCE_FROM_BLOCKIO_THIS(This);
  Media     = &Instance->BlockMedia;
  BlockSize = Media->BlockSize;

  if (MediaId != Media->MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if (Lba > Media->LastBlock) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Lba + (BufferSize / BlockSize) - 1) > Media->LastBlock) {
    return EFI_INVALID_PARAMETER;
  }

  if (BufferSize % BlockSize != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }

  // The eMMC (slot 1) holds the phone's bootloader/Android partitions:
  // refuse every write to it, not just the first partitions
  if (Instance->MmcDev->config.slot <= 1) {
    return EFI_WRITE_PROTECTED;
  }

  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  RC     = mmc_write(Instance, (UINT64)Lba * BlockSize, BufferSize, Buffer);
  gBS->RestoreTPL(OldTpl);

  if (RC == 0)
    return EFI_SUCCESS;
  else
    return EFI_DEVICE_ERROR;
}

EFI_STATUS
EFIAPI
MMCHSFlushBlocks(IN EFI_BLOCK_IO_PROTOCOL *This)
{
  // Nothing required
  return EFI_SUCCESS;
}

VOID EFIAPI MMCHSExitBsUninit(IN EFI_EVENT Event, IN VOID *Context)
{
  EFI_TPL OldTpl;

  BIO_INSTANCE *Instance = (BIO_INSTANCE *)Context;
  ASSERT(Instance != NULL);

  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);

  // Put card into sleep
  //mmc_put_card_to_sleep(Instance->MmcDev);

  gBS->RestoreTPL(OldTpl);
}

EFI_STATUS
BioInstanceContructor(OUT BIO_INSTANCE **NewInstance)
{
  BIO_INSTANCE *Instance;

  Instance = AllocateCopyPool(sizeof(BIO_INSTANCE), &mBioTemplate);
  if (Instance == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Instance->BlockIo.Media = &Instance->BlockMedia;

  *NewInstance = Instance;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MMCHSInitialize(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  // let the target register MMC devices
  LibQcomTargetMmcSdhciInit(PlatformCallbackInitSlot);

  return EFI_SUCCESS;
}
