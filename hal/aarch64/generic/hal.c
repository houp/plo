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


/* Diagnostic instrumentation for Step 3 (plo MMU+caches ON) bisection.
 *
 * Step 3 has failed twice with the same TR3-then-silence hang despite
 * Path A's EL-aware generalisation of mmu.c + cache.c. To localise the
 * remaining EL2-specific trap we re-enable Step 3 here, instrument
 * each sub-step with hal_consolePrint markers, and reorder hal_init
 * so console_init runs BEFORE hal_memoryInit (caches-off PL011 MMIO
 * writes work without MMU).
 *
 * Expected post-fix UART output:
 *   "mem: pre-init\n" / "mem: pre-enable\n" / "mem: post-enable\n"
 *
 * Whichever marker DOES print and which one does NOT will tell us
 * which sysreg write hangs.
 */
static void hal_memoryInit(void)
{
	size_t sz;
	addr_t addr;

	hal_consolePrint("mem: pre-init\n");
	mmu_init();
	hal_consolePrint("mem: post-init\n");

	/* Pi 4 4GB unlock: remap all ARM-accessible DRAM as Normal WB
	 * Cacheable, EXCEPT the 76 MB GPU reserve (which VC4 owns and which
	 * must stay Device so ARM-side cache writes don't alias VC4's
	 * incoherent view of the framebuffer/GPU heap). Everything from
	 * ADDR_DDR through SIZE_DDR is identity-remapped; the GPU hole stays
	 * at the default DEVICE mapping installed by mmu_init.
	 *   0x00000000 - 0x3b3fffff   CACHED  (chunk 1, 948 MB)
	 *   0x3b400000 - 0x3fffffff   DEVICE  (GPU reserve, untouched)
	 *   0x40000000 - 0xfbffffff   CACHED  (chunk 2, 3008 MB)
	 *   0xfc000000 - 0xffffffff   DEVICE  (BCM2711 peripherals)
	 */
	for (sz = 0; sz < (size_t)SIZE_DDR; sz += SIZE_MMU_SECTION_REGION) {
		addr = (addr_t)ADDR_DDR + sz;
		if ((addr >= (addr_t)ADDR_GPU_RSV) &&
				(addr < ((addr_t)ADDR_GPU_RSV + (addr_t)SIZE_GPU_RSV))) {
			continue; /* leave Device — VC4 owns this range */
		}
		mmu_mapAddr(addr, addr, MMU_FLAG_CACHED);
	}
	hal_consolePrint("mem: post-map\n");

	/* Enable MMU + D-cache + I-cache in ONE SCTLR_EL1 write.
	 *
	 * This is the exact recipe used by Circle, rust-raspberrypi-OS-
	 * tutorials, NetBSD/evbarm, and Linux's __enable_mmu on Pi 4. The
	 * key bits identified by parallel multi-subagent research:
	 *
	 *   1. SINGLE SCTLR write, not staged. Staging (M then M|I then
	 *      M|I|C) opens a speculation window matching Cortex-A72
	 *      erratum 1319367 and creates moments where the IFU's
	 *      cacheability decisions are inconsistent with the SCTLR
	 *      state, leading to garbage I-fetches after the third stage.
	 *   2. NO post-flip `ic ialluis`. Per ARMv8 ARM B2.2.7 the cache
	 *      invalidation across the M=0->1 boundary is implicit; an
	 *      explicit broadcast invalidate opens a fresh speculation
	 *      window that NONE of the working bare-metal projects do.
	 *   3. NO pre-MMU set/way (`dc isw`) or per-VA (`dc ivac`) data-
	 *      cache maintenance over the kernel image. ARM ARM B2.2.6 +
	 *      DEN0024 are explicit that set/way is "for power-down only,
	 *      not coherency". Per-VA `dc ivac` over the image was also
	 *      empirically tried — didn't help; it just creates more
	 *      speculation surface.
	 *
	 * The pre-flip ritual (broadcast-IS I-cache + TLB invalidate +
	 * barriers) IS architecturally required and matches Linux's
	 * `set_sctlr` precondition.
	 */
	{
		u64 val;

		hal_consolePrint("mem: pre-iallu\n");
		asm volatile (
			"ic   ialluis\n"
			"dsb  ish\n"
			"tlbi vmalle1is\n"
			"dsb  ish\n"
			"isb\n"
			::: "memory");
		hal_consolePrint("mem: post-iallu\n");

		asm volatile ("mrs %0, sctlr_el1" : "=r"(val));
		hal_consolePrint("mem: post-read-sctlr\n");

		/* SCTLR_EL1.M only (caches off). M-only is the boot-correct
		 * baseline on Pi 4. D-cache enable in plo is a multi-stage
		 * problem (TD-plo-dcache, TD-plo-icache) tracked in
		 * docs/research/2026-05-12-dcache-civac-partial-fix.md and
		 * docs/research/2026-05-13-plo-cache-empirical-pivot.md.
		 *
		 * Empirically confirmed this session:
		 *   - Single-shot M|C hangs at the MSR (A72 quirk,
		 *     independent of erratum 1319367 which IS now applied
		 *     in the armstub).
		 *   - Staged M-then-MC (with ISB between) clears that hang.
		 *   - With staged M|C, plo reaches the banner cleanly
		 *     thanks to 1319367; but the user.plo command parse
		 *     hits intermittent mid-string printf garble and the
		 *     kernel ELF read fails with EINVAL.
		 *   - `dc civac` of firmware-dirty L2 lines is
		 *     COUNTER-PRODUCTIVE: civac cleans (writes back) the
		 *     stale lines on top of correct RAM contents — A72 L2
		 *     is UNIFIED (I+D), so cleaning poisons plo's .text.
		 *     `dc ivac` (invalidate-only) is the correct
		 *     primitive but didn't fully clear the residual
		 *     garble — root cause still partially open.
		 *
		 * The plo cache-on path is parked here. The kernel boots
		 * in its own address space (high VAs that firmware never
		 * touched) and is the better target for cache enable — that
		 * work continues separately. */
		val |= (1uL << 0);
		hal_consolePrint("mem: pre-sctlr-M\n");
		asm volatile (
			"msr sctlr_el1, %0\n"
			"isb\n"
			:: "r"(val) : "memory");
		hal_consolePrint("mem: post-sctlr-M\n");
	}
	hal_consolePrint("mem: post-enable\n");
}


/* SMP smoke test: wake cores 1-3 from their armstub spin-table parking
 * and point them at secondary_smoke_entry (in _init.S). They will
 * print `cN: alive\n` to UART and park in WFE — no kernel state is
 * touched, no shared resource is contended (no stack, no MMU). This
 * proves the wake-up mechanism works end-to-end without any risk to
 * core 0's boot path.
 *
 * Pi 4 armstub spin-table layout (see phoenix-armstub8-rpi4.S near
 * `secondary_spin`):
 *   PA 0xD8 = spin_cpu0  (unused — core 0 doesn't spin)
 *   PA 0xE0 = spin_cpu1
 *   PA 0xE8 = spin_cpu2
 *   PA 0xF0 = spin_cpu3
 * Cores wait at `wfe` then `ldr x4, [spin_cpu0 + coreID*8]`. A non-
 * zero value released by `sev` from any core wakes them and they
 * `br x4` to that address. We point them at secondary_smoke_entry.
 */
extern void secondary_smoke_entry(void);

static void hal_smpBringupSecondaries(void)
{
	addr_t entry = (addr_t)secondary_smoke_entry;

	/* Write spin_cpu1 / spin_cpu2 / spin_cpu3 at PAs 0xE0 / 0xE8 / 0xF0
	 * via inline asm — `str` to a near-zero address tripped GCC's
	 * -Warray-bounds=2 when expressed as a C pointer dereference. */
	asm volatile (
		"mov x10, #0xe0\n"
		"str %0, [x10]\n"        /* spin_cpu1 */
		"mov x10, #0xe8\n"
		"str %0, [x10]\n"        /* spin_cpu2 */
		"mov x10, #0xf0\n"
		"str %0, [x10]\n"        /* spin_cpu3 */
		"dsb sy\n"
		"sev\n"
		:: "r"(entry) : "x10", "memory");
	hal_consolePrint("hal: smp smoke woke cores 1-3\n");
}


void hal_init(void)
{
	interrupts_init();
	console_init();           /* moved BEFORE hal_memoryInit for diag prints */
	hal_consolePrint("hal: console_init done\n");
	hal_memoryInit();
	hal_consolePrint("hal: hal_memoryInit done\n");
	timer_init();
	hal_consolePrint("hal: timer_init done\n");
	video_init();
	hal_consolePrint("hal: video_init done\n");
	hal_printCurrentEl();
	video_markHalReady();
	hal_smpBringupSecondaries();
	hal_consolePrint("hal: init complete\n");

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

	/* Tear down the cacheable execution environment hal_memoryInit() set
	 * up, mirroring the canonical zynqmp pattern at
	 * plo/hal/aarch64/zynqmp/hal.c:255-266. Order: drop D-cache enable
	 * first, civac entire DDR (so no further fills can race the flush),
	 * drop I-cache, invalidate I-cache, then mmu_disable. */
	hal_dcacheEnable(0);
	hal_dcacheFlush((addr_t)ADDR_DDR, (addr_t)ADDR_DDR + (addr_t)SIZE_DDR);
	hal_icacheEnable(0);
	hal_icacheInval();
	mmu_disable();

	hal_probeSyspage();

	/* TD-15 phase 1: stamp mailbox buffer with known pattern; kernel
	 * reads it back via NC alias to detect VC4 writes during the
	 * plo→kernel→main_initthr handoff window. */
	hal_td15ProbeWrite();

	hal_exitToEL1();

	hal_consolePrint("hal: jump returned\n");
	return 0;
}
