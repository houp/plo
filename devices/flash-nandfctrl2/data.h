/*
 * Phoenix-RTOS
 *
 * Operating system loader
 *
 * GRLIB NANDFCTRL2 Data Interface
 *
 * Copyright 2026 Phoenix Systems
 * Author: Lukasz Leczkowski
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef FLASH_NANDFCTRL2_DATA_H_
#define FLASH_NANDFCTRL2_DATA_H_


#include "nandfctrl2.h"


int data_doSync(nand_die_t *nand);


#endif
