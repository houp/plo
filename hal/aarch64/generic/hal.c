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
#include "../mmu.h"
#include "../cache.h"


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


/* Step 3 of the canonical-idiom alignment plan was attempted twice
 * (2026-05-10):
 *
 *   1st attempt: bare hal_memoryInit + plo's mmu.c writing *_EL3 regs.
 *      Hung at relocator's TR3, no plo banner. Diagnosis: plo runs
 *      at EL2 on rpi4b (armstub drops to EL2), EL3 sysreg writes trap.
 *
 *   2nd attempt: Path A (docs/plans/plo-el2-mmu-fix.md) applied —
 *      plo's mmu.c + cache.c generalised to dispatch on currentEL and
 *      write the matching sysreg bank. Same TR3-then-silence hang.
 *      Remaining suspects: TCR_EL2 field layout edge case, sctlr_el2
 *      baseline from armstub, or another sysreg access along
 *      mmu_init / mmu_enable that's not yet generalised.
 *
 * Step 3 deferred until Step 7 (early-boot diagnostic instrumentation
 * — docs/plans/early-boot-diagnostic-instrumentation.md) lands and
 * gives us enough observability to localise the trap.
 *
 * Path A generalization (mmu.c + cache.c EL-aware sysreg writes)
 * REMAINS in tree — it's a clean structural improvement that other
 * Phoenix A-class targets entering at EL2 will benefit from, and it's
 * a no-op for EL3-entering targets like zynqmp.
 *
 * static void hal_memoryInit(void) — disabled, see above.
 */


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


static void hal_printHex64(const char *label, u64 val)
{
	static char buf[20];
	int i;

	hal_consolePrint(label);
	for (i = 15; i >= 0; --i) {
		u8 n = (u8)((val >> (i * 4)) & 0xfu);
		buf[15 - i] = (n < 10u) ? (char)('0' + n) : (char)('a' + n - 10u);
	}
	buf[16] = '\n';
	buf[17] = '\0';
	hal_consolePrint(buf);
}


/* E1 probe: with plo running cache-off (SCTLR.C=0), two back-to-back loads
 * over the same DDR range should return identical bytes unless some other
 * agent (firmware, VideoCore DMA, secondary core) is writing to DDR. Probe
 * the syspage offset region 0x280..0x340 (where the kernel later observes
 * bit-flipped map entries) twice with a dsb between, and report any diffs
 * via UART. If diffs appear, an external writer is proven. */
static void hal_probeSyspage(void)
{
	volatile const u64 *base;
	u64 snap1[24], snap2[24];
	int i, diffs = 0;
	volatile int spin;

	if (hal_common.hs == NULL) {
		hal_consolePrint("probe: no syspage\n");
		return;
	}

	base = (volatile const u64 *)((u8 *)hal_common.hs + 0x280);

	hal_consolePrint("probe: pre-jump read#1\n");
	for (i = 0; i < 24; ++i) {
		snap1[i] = base[i];
	}
	__asm__ volatile("dsb sy" ::: "memory");

	for (spin = 0; spin < 10000; ++spin) {
	}

	__asm__ volatile("dsb sy" ::: "memory");
	for (i = 0; i < 24; ++i) {
		snap2[i] = base[i];
	}
	__asm__ volatile("dsb sy" ::: "memory");

	for (i = 0; i < 24; ++i) {
		if (snap1[i] != snap2[i]) {
			++diffs;
			hal_printHex64("probe diff off=", (u64)(0x280 + i * 8));
			hal_printHex64("  r1=", snap1[i]);
			hal_printHex64("  r2=", snap2[i]);
		}
	}

	if (diffs == 0) {
		hal_consolePrint("probe: no diff (DDR stable)\n");
	}
	else {
		hal_consolePrint("probe: external writer detected\n");
	}

	/* Dump the exact 32 bytes (offset 0x310..0x32F) the kernel B{} field
	 * shows — so we can compare plo's source vs kernel's copy across boots.
	 * If these bytes vary across boots while the kernel's B{} also varies
	 * matching them, plo is feeding uninitialized syspage padding. If
	 * plo's bytes are stable but kernel's vary, the corruption is on the
	 * kernel side. */
	{
		volatile const u64 *p = (volatile const u64 *)((u8 *)hal_common.hs + 0x310);
		hal_printHex64("probe[0x310]=", p[0]);
		hal_printHex64("probe[0x318]=", p[1]);
		hal_printHex64("probe[0x320]=", p[2]);
		hal_printHex64("probe[0x328]=", p[3]);
	}
}


/* TODO(TD-15-mboxprobe): write a known 64-byte pattern to PA
 * PLO_RPI_MAILBOX_BUFFER_ADDRESS just before eret. Kernel reads it
 * back via the NC TTL3 alias added in
 * phoenix-rtos-kernel/hal/aarch64/_init.S; drift = some agent
 * (suspected VC4) is writing to ARM-usable DRAM after our last
 * mailbox call. Pattern: word[i] = 0xa5a5a5a5 ^ (i * 0x01010101).
 * Plo runs cache-off so a direct store hits DDR; we still issue a
 * dsb sy + isb to drain. Remove together with the kernel-side
 * probe once TD-15 phase 1 is done. */
#if defined(PLO_RPI_MAILBOX_BUFFER_ADDRESS) && (PLO_RPI_MAILBOX_BUFFER_ADDRESS != 0)
static void hal_td15ProbeWrite(void)
{
	volatile u32 *buf = (volatile u32 *)(addr_t)PLO_RPI_MAILBOX_BUFFER_ADDRESS;
	u32 i;
	hal_consolePrint("td15: probe write start\n");
	for (i = 0u; i < 16u; ++i) {
		buf[i] = 0xa5a5a5a5u ^ (i * 0x01010101u);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	__asm__ volatile("isb" ::: "memory");
	hal_printHex64("td15: probe write done @ pa=", (u64)PLO_RPI_MAILBOX_BUFFER_ADDRESS);
}
#else
static void hal_td15ProbeWrite(void)
{
	hal_consolePrint("td15: probe write skipped (no mailbox buffer addr)\n");
}
#endif


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
	hal_consolePrint("hal: jump exit el1\n");

	/* Clean+invalidate the entire ARM-usable DDR bank by VA to PoC so every
	 * dirty line plo may have produced reaches DDR before the kernel takes
	 * over (with caches OFF, in the current rpi4b config — Step 3
	 * deferred until early-boot instrumentation localises the
	 * TR3-then-silence hang). The flush is a no-op while plo runs
	 * caches-off; it becomes load-bearing once Step 3 lands.
	 *
	 * Set/way (dc cisw) is documented by ARM as unreliable for
	 * inter-observer coherency, so we keep using civac by VA.
	 */
	hal_dcacheFlush((addr_t)ADDR_DDR, (addr_t)ADDR_DDR + (addr_t)SIZE_DDR);

	hal_probeSyspage();

	/* TD-15 phase 1: stamp mailbox buffer with known pattern; kernel
	 * reads it back via NC alias to detect VC4 writes during the
	 * plo→kernel→main_initthr handoff window. */
	hal_td15ProbeWrite();

	hal_exitToEL1();

	hal_consolePrint("hal: jump returned\n");
	return 0;
}
