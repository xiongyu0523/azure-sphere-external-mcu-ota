#ifndef LITTLEFS_W25Q128
#define LITTLEFS_W25Q128

#include "./littlefs/lfs.h"

extern const struct lfs_config g_w25q128_littlefs_config;

int w25q128_init(void);
void spiflash_test(void);
void littlefs_test(void);

#endif


