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
#ifdef PLO_RPI_ACTLED_DIAG
	mov w0, #4
	bl actled_stage_emit
#endif
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

#ifdef PLO_RPI_ACTLED_DIAG
	mov w0, #5
	bl actled_stage_emit
#endif

	mov x16, #0
	mov x17, #0
	mov x18, #0
	mov x19, #0
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

#ifdef PLO_RPI_ACTLED_DIAG
	mov w0, #6
	bl actled_stage_emit
#endif

	mrs x20, currentEL
#ifdef PLO_RPI_ACTLED_DIAG
	mov w0, #7
	bl actled_stage_emit
#endif
	cmp x20, #0xc
	b.eq start_el3
	cmp x20, #0x8
	b.eq start_el2
	cmp x20, #0x4
	b.eq start_el1
	b start_el_unknown

start_el3:
#ifdef PLO_RPI_ACTLED_DIAG
	mov w0, #8
	bl actled_stage_emit
#endif
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
#ifdef PLO_RPI_ACTLED_DIAG
	mov w0, #9
	bl actled_stage_emit
#endif
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
#ifdef PLO_RPI_ACTLED_DIAG
	mov w0, #10
	bl actled_stage_emit
#endif
	adr x0, _vector_table
	msr vbar_el1, x0

	mrs x0, cpacr_el1
	orr x0, x0, #(0x3 << 20)
	msr cpacr_el1, x0

	ic iallu
	tlbi vmalle1

start_common:
#ifdef PLO_RPI_ACTLED_DIAG
	mov w0, #11
	bl actled_stage_emit
#endif
	dsb sy
	isb

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
#ifdef PLO_RPI_ACTLED_DIAG
	mov w0, #12
	bl actled_stage_emit
#endif
	adr x1, _startc
	br x1


start_el_unknown:
#ifdef PLO_RPI_ACTLED_DIAG
	mov w0, #13
	bl actled_stage_emit
#endif

error_loop:
	b .

.size _start, .-_start


#ifdef PLO_RPI_ACTLED_DIAG
.macro actled_set_on
	ldr x0, =PLO_RPI_ACTLED_GPIO_GPFSEL4
	ldr w1, [x0]
	mov w2, #(7 << PLO_RPI_ACTLED_SHIFT)
	bic w1, w1, w2
	mov w2, #(1 << PLO_RPI_ACTLED_SHIFT)
	orr w1, w1, w2
	str w1, [x0]

	ldr x0, =PLO_RPI_ACTLED_GPIO_GPSET1
	mov w1, #(1 << PLO_RPI_ACTLED_BIT)
	str w1, [x0]
.endm


.macro actled_set_off
	ldr x0, =PLO_RPI_ACTLED_GPIO_GPFSEL4
	ldr w1, [x0]
	mov w2, #(7 << PLO_RPI_ACTLED_SHIFT)
	bic w1, w1, w2
	mov w2, #(1 << PLO_RPI_ACTLED_SHIFT)
	orr w1, w1, w2
	str w1, [x0]

	ldr x0, =PLO_RPI_ACTLED_GPIO_GPCLR1
	mov w1, #(1 << PLO_RPI_ACTLED_BIT)
	str w1, [x0]
.endm


.macro actled_delay_once reg
	ldr \reg, =PLO_RPI_ACTLED_TICK_DELAY
991:
	subs \reg, \reg, #1
	b.ne 991b
.endm


actled_stage_emit:
	mov x12, x30
	mov w11, w0
	actled_set_off
1:
	actled_set_on
	actled_delay_once x9
	actled_set_off
	actled_delay_once x9
	subs w11, w11, #1
	b.ne 1b
	mov w10, #PLO_RPI_ACTLED_GAP_TICKS
2:
	actled_delay_once x9
	subs w10, w10, #1
	b.ne 2b
	mov x30, x12
	ret
#endif


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
