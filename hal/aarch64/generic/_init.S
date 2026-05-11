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
	/* Defense-in-depth: mask all asynchronous classes (DAIF) before
	 * any state writes. Linux + U-Boot rely on a clean precondition
	 * from the prior stage; the rpi4 armstub does not guarantee one. */
	msr daifSet, #0xf

	msr cntvoff_el2, xzr
	mov x0, #0x3
	msr cnthctl_el2, x0
	/* CPTR_EL2 = 0x33ff matches U-Boot + Linux nVHE: FP/SIMD trap
	 * disabled, RES1 bits set. On A72 the hardware forces RES1 bits
	 * on read-back so semantically equivalent to writing 0, but
	 * writing 0 to RES1 is CONSTRAINED-UNPREDICTABLE prior to ARMv8.2. */
	mov x0, #0x33ff
	msr cptr_el2, x0
	/* SCTLR_EL2 baseline 0x30c50838: SCTLR_EL2 RES1 bits for A72 r0p3
	 * (29, 28, 23, 22, 18, 16, 11, 5, 4). Previous baseline 0x30c00838
	 * missed bit 18 in the write; HW forces it on read-back so prior
	 * boots survived, but mainline writes the architecturally correct
	 * value. */
	ldr x0, =0x30c50838
	msr sctlr_el2, x0
	msr sctlr_el1, xzr
	msr vttbr_el2, xzr

	/* MDCR_EL2 / HSTR_EL2 — UNKNOWN at reset on A72. Linux's
	 * __init_el2_debug always clears MDCR_EL2 (no debug traps to
	 * EL2 from EL1); __init_el2_hstr always clears HSTR_EL2 (no
	 * CP15 traps to EL2). Leaving them UNKNOWN can surface as
	 * speculative-debug-uop traps that the CPU may report with
	 * EC=0x00 (matches our observed fault class). */
	msr mdcr_el2, xzr
	msr hstr_el2, xzr

	/* Mirror MIDR_EL1 -> VPIDR_EL2, MPIDR_EL1 -> VMPIDR_EL2 so
	 * any EL1 code (now or after a future drop) reads sane IDs. */
	mrs x0, midr_el1
	msr vpidr_el2, x0
	mrs x0, mpidr_el1
	msr vmpidr_el2, x0

	mov x0, #0b1110011
	msr actlr_el2, x0
	/* HCR_EL2 = RW(31) | ATA(29) | AMO(5) | IMO(4) | FMO(3).
	 * AMO/IMO/FMO route physical SError/IRQ/FIQ to EL2 instead of
	 * EL1. plo runs at EL2 with no EL1 active; without these bits an
	 * SError targets EL1 and lands in CONSTRAINED-UNPREDICTABLE
	 * territory (top hypothesis for the EC=0x00 fault we observe).
	 * Constant 0xa0000038 doesn't fit a single MOV imm; use ldr =. */
	ldr x0, =0xa0000038
	msr hcr_el2, x0

	adr x0, _vector_table
	msr vbar_el2, x0
	isb

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

/* TD-diag: every vector slot prints a unique tag char then halts, so
 * we can SEE which exception class fires instead of silently looping
 * on `b .` or trapping deeper on _exceptions_dispatch's EL3-only sysreg
 * accesses. Tag mapping:
 *   A 000  SP_EL0 sync     B 080  SP_EL0 irq     C 100  SP_EL0 fiq    D 180  SP_EL0 serror
 *   E 200  SPx    sync     F 280  SPx    irq     G 300  SPx    fiq    H 380  SPx    serror
 *   I 400  L-EL64 sync     J 480  L-EL64 irq     K 500  L-EL64 fiq    L 580  L-EL64 serror
 *   M 600  L-EL32 sync     N 680  L-EL32 irq     O 700  L-EL32 fiq    P 780  L-EL32 serror
 * Each slot prints "EXC" then the tag, then halts. After diagnosis we
 * either fix the offending exception cause or restore the original
 * vector table.
 */
.macro exc_tag tag
	uart_putc \tag
	uart_putc '\n'
	b .
.endm

/* Print one ASCII hex nibble for the low 4 bits of w4. */
.macro uart_put_hexnibble
	and w4, w4, #0xf
	cmp w4, #10
	b.lo 992f
	add w4, w4, #('a' - 10)
	b 993f
992:
	add w4, w4, #'0'
993:
	ldr x3, =(PLO_UART0_BASE_ADDRESS + UARTFR_OFFSET)
994:
	ldr w5, [x3]
	tst w5, #UARTFR_TXFF
	b.ne 994b
	ldr x3, =PLO_UART0_BASE_ADDRESS
	str w4, [x3]
.endm

/* Print a 64-bit reg in x6 as 16 hex chars (high nibble first), no newline. */
.globl uart_put_hex64
.type uart_put_hex64, %function
uart_put_hex64:
	mov x7, #60
995:
	lsr x4, x6, x7
	uart_put_hexnibble
	cbz x7, 996f
	sub x7, x7, #4
	b 995b
996:
	ret
.size uart_put_hex64, .-uart_put_hex64

/* Slot-E (Sync at Current EL with SPx) full diagnostic. Reads EL2
 * exception registers (ESR_EL2, ELR_EL2, FAR_EL2, SPSR_EL2) and prints
 * them as hex. ARM ARM D13.2.36: ESR.EC[31:26] decodes the fault class.
 * Most likely cases for our boot:
 *   0x26 = SP alignment fault    (matches the SCTLR_EL2.SA=1 + RES1
 *                                 missing-bits hypothesis)
 *   0x25 = Data abort same EL    (DFSC field then narrows it)
 *   0x2f = SError                 (latent from armstub/VC4)
 *   0x18 = MSR/MRS trap          (would mean dispatcher EL3-reg)
 */
.globl _slot_e_dump
.type _slot_e_dump, %function
_slot_e_dump:
	uart_putc 'E'
	uart_putc '\n'
	uart_putc 'E'
	uart_putc 'S'
	uart_putc 'R'
	uart_putc '='
	mrs x6, esr_el2
	bl uart_put_hex64
	uart_putc '\n'
	uart_putc 'E'
	uart_putc 'L'
	uart_putc 'R'
	uart_putc '='
	mrs x6, elr_el2
	bl uart_put_hex64
	uart_putc '\n'
	uart_putc 'F'
	uart_putc 'A'
	uart_putc 'R'
	uart_putc '='
	mrs x6, far_el2
	bl uart_put_hex64
	uart_putc '\n'
	uart_putc 'S'
	uart_putc 'P'
	uart_putc 'R'
	uart_putc '='
	mrs x6, spsr_el2
	bl uart_put_hex64
	uart_putc '\n'
	uart_putc 'S'
	uart_putc 'C'
	uart_putc 'T'
	uart_putc '='
	mrs x6, sctlr_el2
	bl uart_put_hex64
	uart_putc '\n'

	/* TTBR0_EL2 / TCR_EL2 / MAIR_EL2 — verify translation setup. */
	uart_putc 'T'
	uart_putc 'T'
	uart_putc 'B'
	uart_putc '='
	mrs x6, ttbr0_el2
	bl uart_put_hex64
	uart_putc '\n'
	uart_putc 'T'
	uart_putc 'C'
	uart_putc 'R'
	uart_putc '='
	mrs x6, tcr_el2
	bl uart_put_hex64
	uart_putc '\n'
	uart_putc 'M'
	uart_putc 'A'
	uart_putc 'R'
	uart_putc '='
	mrs x6, mair_el2
	bl uart_put_hex64
	uart_putc '\n'

	/* Read 4 instruction bytes at ELR_EL2. If these match the static
	 * disassembly (0x911c6000 for `add x0, x0, #0x718`) the I-cache is
	 * NOT the culprit and EC=0 must come from elsewhere. If they
	 * don't match, I-cache aliasing is proven. */
	uart_putc 'I'
	uart_putc 'N'
	uart_putc 'S'
	uart_putc '='
	mrs x8, elr_el2
	ldr w6, [x8]                /* writing w6 zero-extends to x6 */
	bl uart_put_hex64
	uart_putc '\n'
	b .
.size _slot_e_dump, .-_slot_e_dump

.align 11
.globl _vector_table
_vector_table:
.org(_vector_table)
	exc_tag 'A'

.org (_vector_table + 0x80)
	exc_tag 'B'

.org (_vector_table + 0x100)
	exc_tag 'C'

.org (_vector_table + 0x180)
	exc_tag 'D'

.org (_vector_table + 0x200)
	b _slot_e_dump

.org (_vector_table + 0x280)
	exc_tag 'F'

.org (_vector_table + 0x300)
	exc_tag 'G'

.org (_vector_table + 0x380)
	exc_tag 'H'

.org (_vector_table + 0x400)
	exc_tag 'I'

.org (_vector_table + 0x480)
	exc_tag 'J'

.org (_vector_table + 0x500)
	exc_tag 'K'

.org (_vector_table + 0x580)
	exc_tag 'L'

.org (_vector_table + 0x600)
	exc_tag 'M'

.org (_vector_table + 0x680)
	exc_tag 'N'

.org (_vector_table + 0x700)
	exc_tag 'O'

.org (_vector_table + 0x780)
	exc_tag 'P'

.org (_vector_table + 0x800)
