/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#ifndef EXTMCU_HAL_H
#define EXTMCU_HAL_H

#include <stdint.h>
#include <stdbool.h>

void ExtMCU_Init(void);
uint32_t ExtMCU_GetVersion();
bool ExtMCU_Download();

#endif