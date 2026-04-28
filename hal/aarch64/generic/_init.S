/*
 * Phoenix-RTOS
 *
 * Operating system loader
 *
 * Low-level initialization for generic AArch64
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#define __ASSEMBLY__

#include "config.h"
#include "../cpu.h"

.section .init, "ax"

#define UARTFR_OFFSET 0x18
#define UARTFR_TXFF   (1 << 5)

.extern hal_common
.extern hal_coreJumpFlag
.extern hal_firmwareDtb


.macro uart_putc imm
991:
	ldr x3, =(PLO_UART0_BASE_ADDRESS + UARTFR_OFFSET)
	ldr w4, [x3]
	tst w4, #UARTFR_TXFF
	b.ne 991b
	ldr x3, =PLO_UART0_BASE_ADDRESS
	mov w4, #\imm
	str w4, [x3]
.endm


.globl _start
.type _start, %function
_start:
	b _start_after_signature


_start_signature:
	.word PLO_RPI_FIXED_ENTRY_SIGNATURE0
	.word PLO_RPI_FIXED_ENTRY_SIGNATURE1


_start_after_signature:
	mov x19, x0
	b _start_real


_start_real:
	mov x0, #0
	mov x1, #0
	mov x2, #0
	mov x3, #0
	mov x4, #0
	mov x5, #0
	mov x6, #0
	mov x7, #0

	mov x8, #0
	mov x9, #0
	mov x10, #0
	mov x11, #0
	mov x12, #0
	mov x13, #0
	mov x14, #0
	mov x15, #0

	mov x16, #0
	mov x17, #0
	mov x18, #0
	mov x20, #0
	mov x21, #0
	mov x22, #0
	mov x23, #0

	mov x24, #0
	mov x25, #0
	mov x26, #0
	mov x27, #0
	mov x28, #0
	mov x29, #0
	mov x30, #0

	dsb sy
	isb

	mrs x20, currentEL
	cmp x20, #0xc
	b.eq start_el3
	cmp x20, #0x8
	b.eq start_el2
	cmp x20, #0x4
	b.eq start_el1
	b start_el_unknown

start_el3:
	ldr x0, =0x30c50838
	msr sctlr_el3, x0

	msr cptr_el3, xzr

	mov x0, #0b1110011
	msr actlr_el3, x0

	msr cntvoff_el2, xzr
	mov x0, #0x3
	msr cnthctl_el2, x0
	msr cptr_el2, xzr
	ldr x0, =0x30c00838
	msr sctlr_el2, x0
	msr sctlr_el1, xzr
	msr vttbr_el2, xzr

	mov x0, #0b1110011
	msr actlr_el2, x0
	mov x0, #((1 << 31) | (1 << 29))
	msr hcr_el2, x0

	mov x0, #0x53e
	msr scr_el3, x0

	adr x0, _vector_table
	msr vbar_el3, x0

	ic iallu
	tlbi alle3

	b start_common


start_el2:
	msr cntvoff_el2, xzr
	mov x0, #0x3
	msr cnthctl_el2, x0
	msr cptr_el2, xzr
	ldr x0, =0x30c00838
	msr sctlr_el2, x0
	msr sctlr_el1, xzr
	msr vttbr_el2, xzr

	mov x0, #0b1110011
	msr actlr_el2, x0
	mov x0, #((1 << 31) | (1 << 29))
	msr hcr_el2, x0

	adr x0, _vector_table
	msr vbar_el2, x0

	ic iallu
	tlbi alle2

	b start_common


start_el1:
	adr x0, _vector_table
	msr vbar_el1, x0

	mrs x0, cpacr_el1
	orr x0, x0, #(0x3 << 20)
	msr cpacr_el1, x0

	ic iallu
	tlbi vmalle1


start_common:
	dsb sy
	isb

	/* TD-05: drop all data-cache lines across every level up to LoC via
	 * set/way invalidate-only (dc isw), before plo writes anything. Pi 4
	 * firmware runs with D-cache enabled and leaves dirty lines for
	 * arbitrary DDR PAs. plo runs cache-off (SCTLR.{M,C,I}=0), so plo's
	 * stores go directly to DDR. But firmware's dirty lines (in L1 AND
	 * L2, since A72's L2 is unified and not fully inclusive of L1) stay
	 * resident and get evicted later (speculatively, once the kernel
	 * enables D-cache) on top of plo's DDR data, producing bit-level
	 * nondeterminism in the syspage bytes the kernel then copies.
	 * Invalidate-only (not civac) is required: civac would write
	 * firmware's stale lines back to DDR, clobbering plo's fresh uncached
	 * writes. Set/way (not VA) because A72 promotes `dc ivac` to civac on
	 * writable addresses. Safe to drop all lines here: plo reinitializes
	 * BSS and heap itself, so no reachable data needs to survive. */
	mrs x16, clidr_el1
	and x17, x16, #0x7000000  /* LoC << 24 */
	lsr x17, x17, #23         /* LoC * 2 (csselr level step) */
	cbz x17, 110f             /* no cache at all? skip */
	mov x18, #0               /* current csselr value (level<<1 | InD) */
102:
	msr csselr_el1, x18
	isb
	mrs x0, ccsidr_el1
	and x2, x0, #0x7          /* log2(line size in words) - 2 */
	add x2, x2, #4            /* log2(line size in bytes) */
	ubfx x3, x0, #3, #10      /* associativity - 1 */
	ubfx x4, x0, #13, #15     /* number of sets - 1 */
	clz w5, w3                /* bit position for way in set/way operand */
100:
	mov x6, x4
101:
	lsl x7, x3, x5
	lsl x8, x6, x2
	orr x7, x7, x8
	orr x7, x7, x18           /* level bits [3:1] */
	dc isw, x7
	subs x6, x6, #1
	b.ge 101b
	subs x3, x3, #1
	b.ge 100b
	add x18, x18, #2          /* next level */
	cmp x18, x17
	b.lt 102b
110:
	dsb sy
	isb

	ldr x2, =hal_firmwareDtb
	str x19, [x2]

	adr x1, _stack
	mov sp, x1

	/* Trap all cores except core 0 until the kernel handoff. */
	adr x1, hal_coreJumpFlag
	str xzr, [x1]
	mrs x0, mpidr_el1
	and x0, x0, #0xff
	cbz x0, start_core_0
other_core_trap:
	dsb ish
	wfe
	/*
	 * Keep secondary CPUs parked until generic AArch64 grows an SMP-capable
	 * kernel target. The current generic kernel configuration is single-core.
	 */
	b other_core_trap

start_core_0:
	adr x1, _startc
	br x1


start_el_unknown:

error_loop:
	b .

.size _start, .-_start


.align 4
.globl hal_exitToEL1
.type hal_exitToEL1, %function
hal_exitToEL1:
	dsb sy
	isb
	uart_putc 10
	uart_putc 65
	adr x0, hal_common
	ldp x9, x0, [x0]
	mrs x1, currentEL
	cmp x1, #0xc
	b.eq exit_el3
	cmp x1, #0x8
	b.eq exit_el2
	cmp x1, #0x4
	b.eq exit_el1
	b error_loop

exit_el3:
	uart_putc 51
	uart_putc 10
	msr elr_el3, x0
	mov x0, #0b1111000101
	msr spsr_el3, x0
	mov x2, #0x5b1
	msr scr_el3, x2
	eret

exit_el2:
	uart_putc 50
	uart_putc 10
	msr elr_el2, x0
	mov x0, #0b1111000101
	msr spsr_el2, x0
	eret

exit_el1:
	uart_putc 49
	uart_putc 10
	br x0
.size hal_exitToEL1, .-hal_exitToEL1


#include "../_interrupts.S"
#include "../_exceptions.S"

.align 11
.globl _vector_table
_vector_table:
.org(_vector_table)
	b .

.org (_vector_table + 0x80)
	b .

.org (_vector_table + 0x100)
	b .

.org (_vector_table + 0x180)
	b .

.org (_vector_table + 0x200)
	b _exceptions_dispatch

.org (_vector_table + 0x280)
	b _interrupts_dispatch

.org (_vector_table + 0x300)
	b _interrupts_dispatch

.org (_vector_table + 0x380)
	b _exceptions_dispatch

.org (_vector_table + 0x400)
	b .

.org (_vector_table + 0x480)
	b .

.org (_vector_table + 0x500)
	b .

.org (_vector_table + 0x580)
	b .

.org (_vector_table + 0x600)
	b .

.org (_vector_table + 0x680)
	b .

.org (_vector_table + 0x700)
	b .

.org (_vector_table + 0x780)
	b .

.org (_vector_table + 0x800)
