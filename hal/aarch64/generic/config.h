/*
 * Phoenix-RTOS
 *
 * Operating system loader
 *
 * Platform configuration file for generic AArch64
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _CONFIG_H_
#define _CONFIG_H_

#define GICD_BASE_ADDRESS ((void *)0x08000000)
#define GICC_BASE_ADDRESS ((void *)0x08010000)

#define UART0_BASE_ADDRESS ((void *)0x09000000)

#define RAM_ADDR      0x48000000
#define RAM_BANK_SIZE 0x08000000

#ifndef __ASSEMBLY__

#include "types.h"

typedef struct {
	long long int resetReason;
} __attribute__((packed)) hal_syspage_t;

#include <phoenix/syspage.h>

#include "../cpu.h"

#define PATH_KERNEL "phoenix-aarch64a53-generic.elf"

#endif

#include "ld/aarch64a53-generic.ldt"

#define ADDR_KERNEL (ADDR_PLO + SIZE_PLO)

#endif
