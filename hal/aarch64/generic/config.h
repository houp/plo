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

#include <board_config.h>

#ifndef PLO_GICD_BASE_ADDRESS
#define PLO_GICD_BASE_ADDRESS 0x08000000u
#endif

#ifndef PLO_GICC_BASE_ADDRESS
#define PLO_GICC_BASE_ADDRESS 0x08010000u
#endif

#ifndef PLO_UART0_BASE_ADDRESS
#define PLO_UART0_BASE_ADDRESS 0x09000000u
#endif

#define GICD_BASE_ADDRESS ((void *)PLO_GICD_BASE_ADDRESS)
#define GICC_BASE_ADDRESS ((void *)PLO_GICC_BASE_ADDRESS)

#define UART0_BASE_ADDRESS ((void *)PLO_UART0_BASE_ADDRESS)

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
