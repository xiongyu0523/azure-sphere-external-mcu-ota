#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <assert.h>

#include "applibs_versions.h"

#include <applibs/log.h>
#include <applibs/spi.h>
#include <applibs/gpio.h>
#include <hw/sample_hardware.h>

#include "delay.h"
#include "spiflash_driver/src/spiflash.h"
#include "littlefs/lfs.h"
#include "littlefs/lfs_util.h"

#define W25Q128_PAGE_SIZE     (256)
#define W25Q128_SECTOR_SIZE   (16 * W25Q128_PAGE_SIZE)
#define W25Q128_BLOCK_SIZE    (16 * W25Q128_SECTOR_SIZE)
#define W25Q128_TOTAL_SIZE    (256 * W25Q128_BLOCK_SIZE)

static int spiFd = 0;
static int gpioFd = 0;

int azsphere_spiflash_spi_txrx(struct spiflash_s* spi, const uint8_t* tx_data, uint32_t tx_len, uint8_t* rx_data, uint32_t rx_len);
void azsphere_spiflash_spi_cs(struct spiflash_s* spi, uint8_t cs);
void azsphere_spiflash_wait(struct spiflash_s* spi, uint32_t ms);

static const spiflash_cmd_tbl_t common_spiflash_cmds = SPIFLASH_CMD_TBL_STANDARD;

static const spiflash_config_t w25q128jv_spiflash_config = {
    .sz = W25Q128_TOTAL_SIZE,
    .page_sz = W25Q128_PAGE_SIZE,
    .addr_sz = 3,
    .addr_dummy_sz = 0,
    .addr_endian = SPIFLASH_ENDIANNESS_BIG,
    .sr_write_ms = 10,
    .page_program_ms = 3,
    .block_erase_4_ms = 45,
    .block_erase_8_ms = 0, // not supported
    .block_erase_16_ms = 0, // not supported
    .block_erase_32_ms = 120,
    .block_erase_64_ms = 150,
    .chip_erase_ms = 40000
};

static const spiflash_hal_t azsphere_spiflash_hal = {
    ._spiflash_spi_txrx = azsphere_spiflash_spi_txrx,
    ._spiflash_spi_cs = azsphere_spiflash_spi_cs,
    ._spiflash_wait = azsphere_spiflash_wait
};

static spiflash_t spiflash;

int w25q128_init(void)
{
    int ret;
    SPIMaster_Config config;

    ret = SPIMaster_InitConfig(&config);
    if (ret < 0) {
        Log_Debug("ERROR: SPIMaster_InitConfig: errno=%d (%s)\r\n", errno, strerror(errno));
        return -1;
    }

    config.csPolarity = SPI_ChipSelectPolarity_ActiveLow;
    spiFd = SPIMaster_Open(FLASH_SPI, MT3620_SPI_CS_A, &config);
    if (spiFd < 0) {
        Log_Debug("ERROR: SPIMaster_Open: errno=%d (%s)\r\n", errno, strerror(errno));
        return -1;
    }

    ret = SPIMaster_SetBusSpeed(spiFd, 8000000);
    if (ret < 0) {
        Log_Debug("ERROR: SPIMaster_SetBusSpeed: errno=%d (%s)\r\n", errno, strerror(errno));
        close(spiFd);
        return -1;
    }

    ret = SPIMaster_SetMode(spiFd, SPI_Mode_0);
    if (ret < 0) {
        Log_Debug("ERROR: SPIMaster_SetMode: errno=%d (%s)\r\n", errno, strerror(errno));
        close(spiFd);
        return -1;
    }

    gpioFd = GPIO_OpenAsOutput(FLASH_CS, GPIO_OutputMode_PushPull, GPIO_Value_High);
    if (gpioFd < 0) {
        Log_Debug("ERROR: GPIO_OpenAsOutput: errno=%d (%s)\n", errno, strerror(errno));
        close(spiFd);
        return -1;
    }

    SPIFLASH_init(&spiflash,
        &w25q128jv_spiflash_config,
        &common_spiflash_cmds,
        &azsphere_spiflash_hal,
        NULL, SPIFLASH_SYNCHRONOUS, NULL);
}

int azsphere_spiflash_spi_txrx(struct spiflash_s* spi, const uint8_t* tx_data, uint32_t tx_len, uint8_t* rx_data, uint32_t rx_len)
{
    (void)spi;

    // safety check to avoid write complex driver to handle more than 4096 bytes in a single transfer
    if ((tx_len > 4096) || (rx_len > 4096)) {
        Log_Debug("ERROR: SPIMaster_TransferSequential transfer up to 4096 bytes in each direction");
        return -1;
    }

    int ret;
    SPIMaster_Transfer transfers;

    ret = SPIMaster_InitTransfers(&transfers, 1);
    if (ret < 0) {
        return -1;
    }

    if (tx_len > 0) {

        transfers.flags = SPI_TransferFlags_Write;
        transfers.writeData = tx_data;
        transfers.readData = NULL;
        transfers.length = tx_len;

        ret = SPIMaster_TransferSequential(spiFd, &transfers, 1);
    }

    if ((ret > 0) && (rx_len > 0)) {

        transfers.flags = SPI_TransferFlags_Read;
        transfers.writeData = NULL;
        transfers.readData = rx_data;
        transfers.length = rx_len;

        ret = SPIMaster_TransferSequential(spiFd, &transfers, 1);
    }

    return ret > 0 ? 0 : -1;
}

void azsphere_spiflash_spi_cs(struct spiflash_s* spi, uint8_t cs)
{
    (void)spi;

    if (cs) {
        // assert cs pin
        GPIO_SetValue(gpioFd, GPIO_Value_Low);
    } else {
        // de assert cs pin
        GPIO_SetValue(gpioFd, GPIO_Value_High);
    }
}

void azsphere_spiflash_wait(struct spiflash_s* spi, uint32_t ms)
{
    (void)spi;

    delay_ms(ms);
}

static int flash_read_wrapper(const struct lfs_config* c, lfs_block_t block, lfs_off_t off, void* buffer, lfs_size_t size) 
{
    return SPIFLASH_read(&spiflash, block * c->block_size + off, size, buffer) == SPIFLASH_OK ? LFS_ERR_OK : LFS_ERR_IO;
}

int flash_program_wrapper(const struct lfs_config* c, lfs_block_t block, lfs_off_t off, const void* buffer, lfs_size_t size) 
{
    return SPIFLASH_write(&spiflash, block * c->block_size + off, size, buffer) == SPIFLASH_OK ? LFS_ERR_OK : LFS_ERR_IO;
}

int flash_erase_wrapper(const struct lfs_config* c, lfs_block_t block)
{
    return SPIFLASH_erase(&spiflash, block * c->block_size, 4096) == SPIFLASH_OK ? LFS_ERR_OK : LFS_ERR_IO;
}

int flash_sync_wrapper(const struct lfs_config* c)
{
    return LFS_ERR_OK;
}

const struct lfs_config g_w25q128_littlefs_config = {
    // block device operations
    .read = flash_read_wrapper,
    .prog = flash_program_wrapper,
    .erase = flash_erase_wrapper,
    .sync = flash_sync_wrapper,
    .read_size = 16,
    .prog_size = W25Q128_PAGE_SIZE,
    .block_size = W25Q128_SECTOR_SIZE,
    .block_count = W25Q128_TOTAL_SIZE / W25Q128_SECTOR_SIZE,
    .block_cycles = 500,
    .cache_size = W25Q128_PAGE_SIZE,
    .lookahead_size = 16,
};

void spiflash_test(void)
{
    uint8_t wbuf[256], rbuf[256];

    uint32_t jedec = 0;
    assert(SPIFLASH_read_jedec_id(&spiflash, &jedec) == SPIFLASH_OK);
    assert(jedec == 0x001840EF);
    Log_Debug("JEDEC ID = 0x%X\n", jedec);

    // Test Erase 
    assert(SPIFLASH_erase(&spiflash, 0, 8192) == SPIFLASH_OK);
    assert(SPIFLASH_erase(&spiflash, 2048, 4096) == SPIFLASH_ERR_ERASE_UNALIGNED);
    assert(SPIFLASH_erase(&spiflash, 4096, 2048) == SPIFLASH_ERR_ERASE_UNALIGNED);
    assert(SPIFLASH_read(&spiflash, 0, 256, &rbuf[0]) == SPIFLASH_OK);
    Log_Debug("Data after erase\n");
    for (uint32_t i = 0; i < 256; i++) {
        Log_Debug("0x%02X ", rbuf[i]);
    }
    Log_Debug("\n");


    // Test aligned & single page write & read
    for (uint32_t i = 0; i < 256; i++) {
        wbuf[i] = i;
    }

    assert(SPIFLASH_write(&spiflash, 0, 256, &wbuf[0]) == SPIFLASH_OK);
    assert(SPIFLASH_read(&spiflash, 0, 256, &rbuf[0]) == SPIFLASH_OK);

    Log_Debug("Data after write\n");
    for (uint32_t i = 0; i < 256; i++) {
        Log_Debug("0x%02X ", rbuf[i]);
        if (wbuf[i] != rbuf[i]) {
            Log_Debug("Error Detect");
            return;
        }
    }
    Log_Debug("\n");

    // Test unaligned & accross page
    for (uint32_t i = 0; i < 256; i++) {
        wbuf[i] = 255 - i;
    }
    assert(SPIFLASH_write(&spiflash, 489, 65, &wbuf[0]) == SPIFLASH_OK);
    assert(SPIFLASH_read(&spiflash, 489, 65, &rbuf[0]) == SPIFLASH_OK);
    Log_Debug("Data after unaligned write\n");
    for (uint32_t i = 0; i < 65; i++) {
        Log_Debug("0x%02X ", rbuf[i]);
        if (wbuf[i] != rbuf[i]) {
            Log_Debug("Error Detect");
            return;
        }
    }
    Log_Debug("\n");
}

void littlefs_test(void)
{
    lfs_t lfs;
    lfs_file_t file;
    char *content = "Test";
    char buffer[512] = { 0 };
    
    if (lfs_mount(&lfs, &g_w25q128_littlefs_config) != LFS_ERR_OK) {
        Log_Debug("Format and Mount\n");
        assert(lfs_format(&lfs, &g_w25q128_littlefs_config) == LFS_ERR_OK);
        assert(lfs_mount(&lfs, &g_w25q128_littlefs_config) == LFS_ERR_OK);
    }
    assert(lfs_file_open(&lfs, &file, "test.txt", LFS_O_RDWR | LFS_O_CREAT) == LFS_ERR_OK);
    assert(lfs_file_write(&lfs, &file, content, strlen(content)) == 4);
    assert(lfs_file_seek(&lfs, &file, 0, LFS_SEEK_SET) == 0);
    assert(lfs_file_read(&lfs, &file, buffer, 512) == 4);
    Log_Debug("Read content = %s\n", buffer);
    assert(lfs_file_close(&lfs, &file) == LFS_ERR_OK);
    assert(lfs_unmount(&lfs) == LFS_ERR_OK);
}