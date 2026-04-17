/*
 * Phoenix-RTOS
 *
 * Operating system loader
 *
 * Hardware Abstraction Layer
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <hal/hal.h>

#include "../cpu.h"


struct {
	/* These fields are used in assembly code in _init.S, don't reorder them */
	hal_syspage_t *hs;
	addr_t entry;
} hal_common;

volatile u64 hal_coreJumpFlag;
volatile addr_t hal_firmwareDtb;


/* Linker symbols */
extern char __init_start[], __init_end[];
extern char __text_start[], __etext[];
extern char __rodata_start[], __rodata_end[];
extern char __cmd_start[], __cmd_end[];
extern char __init_array_start[], __init_array_end[];
extern char __fini_array_start[], __fini_array_end[];
extern char __ramtext_start[], __ramtext_end[];
extern char __data_start[], __data_end[];
extern char __bss_start[], __bss_end[];
extern char __heap_base[], __heap_limit[];
extern char __stack_top[], __stack_limit[];


extern void console_init(void);
extern void interrupts_init(void);
extern void timer_init(void);
extern void timer_done(void);
extern void video_init(void);
extern void video_publishGraphmode(void);
extern void video_markHalReady(void);
extern void video_markKernelJump(void);
extern void video_markKernelHandoff(void);
extern void hal_exitToEL1(void) __attribute__((noreturn));


static void hal_printCurrentEl(void)
{
	switch (sysreg_read(currentEL)) {
		case 0xc:
			hal_consolePrint("hal: entry EL3\n");
			break;

		case 0x8:
			hal_consolePrint("hal: entry EL2\n");
			break;

		case 0x4:
			hal_consolePrint("hal: entry EL1\n");
			break;

		default:
			hal_consolePrint("hal: entry EL?\n");
			break;
	}
}


static u32 hal_readBe32(addr_t addr)
{
	volatile const u8 *ptr = (const void *)addr;

	return ((u32)ptr[0] << 24) | ((u32)ptr[1] << 16) | ((u32)ptr[2] << 8) | (u32)ptr[3];
}


void hal_init(void)
{
	interrupts_init();
	timer_init();
	console_init();
	video_init();
	hal_printCurrentEl();
	video_markHalReady();

	hal_common.entry = (addr_t)-1;
}


void hal_done(void)
{
	timer_done();
}


void hal_graphicsInit(void)
{
	video_publishGraphmode();
}


void hal_syspageSet(hal_syspage_t *hs)
{
	hal_common.hs = hs;
	hs->resetReason = 0;
	hs->firmwareDtb = 0;
	hs->firmwareDtbSize = 0;

	if ((hal_firmwareDtb != 0u) && (hal_readBe32(hal_firmwareDtb) == 0xd00dfeedu)) {
		hs->firmwareDtb = hal_firmwareDtb;
		hs->firmwareDtbSize = hal_readBe32(hal_firmwareDtb + 4u);
	}
}


const char *hal_cpuInfo(void)
{
	return CPU_INFO;
}


addr_t hal_kernelGetAddress(addr_t addr)
{
	addr_t offs;

	if ((addr_t)VADDR_KERNEL_INIT != (addr_t)ADDR_KERNEL) {
		offs = addr - VADDR_KERNEL_INIT;
		addr = ADDR_KERNEL + offs;
	}

	return addr;
}


void hal_kernelGetEntryPointOffset(addr_t *off, int *indirect)
{
	*off = 0;
	*indirect = 1;
}


void hal_kernelEntryPoint(addr_t addr)
{
	hal_common.entry = addr;
}


int hal_memoryAddMap(addr_t start, addr_t end, u32 attr, u32 mapId)
{
	(void)start;
	(void)end;
	(void)attr;
	(void)mapId;

	return 0;
}


static void hal_getMinOverlappedRange(addr_t start, addr_t end, mapent_t *entry, mapent_t *minEntry)
{
	if ((start < entry->end) && (end > entry->start)) {
		if (start > entry->start) {
			entry->start = start;
		}

		if (end < entry->end) {
			entry->end = end;
		}

		if (entry->start < minEntry->start) {
			minEntry->start = entry->start;
			minEntry->end = entry->end;
			minEntry->type = entry->type;
		}
	}
}


int hal_memoryGetNextEntry(addr_t start, addr_t end, mapent_t *entry)
{
	int i;
	mapent_t tempEntry, minEntry;

	static const mapent_t entries[] = {
		{ .start = (addr_t)__init_start, .end = (addr_t)__init_end, .type = hal_entryTemp },
		{ .start = (addr_t)__text_start, .end = (addr_t)__etext, .type = hal_entryTemp },
		{ .start = (addr_t)__rodata_start, .end = (addr_t)__rodata_end, .type = hal_entryTemp },
		{ .start = (addr_t)__cmd_start, .end = (addr_t)__cmd_end, .type = hal_entryTemp },
		{ .start = (addr_t)__init_array_start, .end = (addr_t)__init_array_end, .type = hal_entryTemp },
		{ .start = (addr_t)__fini_array_start, .end = (addr_t)__fini_array_end, .type = hal_entryTemp },
		{ .start = (addr_t)__ramtext_start, .end = (addr_t)__ramtext_end, .type = hal_entryTemp },
		{ .start = (addr_t)__data_start, .end = (addr_t)__data_end, .type = hal_entryTemp },
		{ .start = (addr_t)__bss_start, .end = (addr_t)__bss_end, .type = hal_entryTemp },
		{ .start = (addr_t)__heap_base, .end = (addr_t)__heap_limit, .type = hal_entryTemp },
		{ .start = (addr_t)__stack_limit, .end = (addr_t)__stack_top, .type = hal_entryTemp },
	};

	if (start == end) {
		return -1;
	}

	minEntry.start = (addr_t)-1;
	minEntry.end = 0;
	minEntry.type = 0;

	tempEntry.start = (addr_t)hal_common.hs;
	tempEntry.end = (addr_t)__heap_limit;
	tempEntry.type = hal_entryReserved;
	hal_getMinOverlappedRange(start, end, &tempEntry, &minEntry);

	for (i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
		if (entries[i].start >= entries[i].end) {
			continue;
		}

		tempEntry.start = entries[i].start;
		tempEntry.end = entries[i].end;
		tempEntry.type = entries[i].type;
		hal_getMinOverlappedRange(start, end, &tempEntry, &minEntry);
	}

	if (minEntry.start != (addr_t)-1) {
		entry->start = minEntry.start;
		entry->end = minEntry.end;
		entry->type = minEntry.type;

		return 0;
	}

	return -1;
}


void hal_cpuReboot(void)
{
	for (;;) {
		hal_cpuHalt();
	}
}


int hal_cpuJump(void)
{
	if (hal_common.entry == (addr_t)-1) {
		hal_consolePrint("hal: jump no entry\n");
		return -1;
	}

	hal_consolePrint("hal: jump entry\n");
	video_markKernelJump();
	hal_interruptsDisableAll();
	hal_consolePrint("hal: jump irq off\n");
	hal_coreJumpFlag = 1;
	video_markKernelHandoff();
	hal_consolePrint("hal: jump exit el1\n");
	hal_exitToEL1();

	hal_consolePrint("hal: jump returned\n");
	return 0;
}
