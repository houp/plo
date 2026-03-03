/*
 * Phoenix-RTOS
 *
 * Operating system loader
 *
 * GRLIB NANDFCTRL2 flash driver
 *
 * Copyright 2026 Phoenix Systems
 * Author: Lukasz Leczkowski
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <hal/hal.h>
#include <lib/lib.h>

#include "nandfctrl2.h"
#include "onfi-4.h"


#define NAND_BBM_SIZE 2 /* Bad Block Marker size in bytes */


/* ======================== Register Definitions ======================== */


/* Core control 0 register */
#define CTRL0_EE         (1U << 13)               /* EDAC enable */
#define CTRL0_RE         (1U << 12)               /* Data randomization enable */
#define CTRL0_LLM        (1U << 10)               /* Linked list mode enable */
#define CTRL0_BBM        (1U << 9)                /* Preserve Bad Block Marking */
#define CTRL0_EDO        (1U << 7)                /* EDO mode enable */
#define CTRL0_IFSEL_SHFT 5                        /* Data Interface select shift */
#define CTRL0_IFSEL_MSK  (3U << CTRL0_IFSEL_SHFT) /* Data Interface select mask */
#define CTRL0_MSEL_SHFT  3                        /* Memory select shift */
#define CTRL0_MSEL_MSK   (3U << CTRL0_MSEL_SHFT)  /* Memory select mask */

/* Core control 2 register */
#define CTRL2_STOP_LL (1U << 3) /* Stop Linked List */
#define CTRL2_ABORT   (1U << 2) /* Abort ongoing execution */
#define CTRL2_DT      (1U << 1) /* Descriptor trigger */
#define CTRL2_RST     (1U << 0) /* Software reset (APB registers) */

/* Core status 0 register */
#define STS0_DA  (1U << 3) /* Descriptor active */
#define STS0_RDY (1U << 0) /* Ready state (set when ready) */

/* Core status 1 register */
#define STS1_STOPLL (1U << 9) /* Stop linked list interrupt */
#define STS1_ABORT  (1U << 8) /* Abort interrupt (set when abort completes) */
#define STS1_TMOUT  (1U << 6) /* Ready/Busy timeout */
#define STS1_CMD    (1U << 5) /* Invalid command */
#define STS1_DS     (1U << 1) /* Descriptor finished */

/* Descriptor command register */
#define DCMD_CMD2_SHFT 24
#define DCMD_CMD1_SHFT 16
#define DCMD_PRECH     (1U << 12) /* Pre-cache */
#define DCMD_DD        (1U << 11) /* Data DMA Disable (1 = APB, 0 = DMA) */
#define DCMD_ED        (1U << 10) /* EDAC Disable for this descriptor */
#define DCMD_RD        (1U << 9)  /* Randomization Disable for this descriptor */
#define DCMD_SRB       (1U << 6)  /* Skip Ready/Busy wait */
#define DCMD_SA        (1U << 5)  /* Skip Address phase */
#define DCMD_SD        (1U << 4)  /* Skip Data phase */
#define DCMD_SC2       (1U << 3)  /* Skip second command phase */
#define DCMD_WARDY     (1U << 2)  /* Wait ARDY */
#define DCMD_IRQ_DS    (1U << 1)  /* Descriptor interrupt enable */
#define DCMD_EN        (1U << 0)  /* Descriptor Enable */

/* Descriptor row & control register */
#define DROW_TAGEN (1U << 24) /* Tag enable */

/* Descriptor column & size register */
#define DCOLSIZE_SIZE_SHFT 16 /* Transfer size in bytes */

/* Capability 1 register */
#define CAP1_E1CAP_SHFT   26
#define CAP1_E1CHUNK_SHFT 21
#define CAP1_E1GF_SHFT    16
#define CAP1_E0CAP_SHFT   10
#define CAP1_E0CHUNK_SHFT 5
#define CAP1_E0GF_SHFT    0

#define CAP1_E1CAP   (0x3fU << CAP1_E1CAP_SHFT)
#define CAP1_E1CHUNK (0x1fU << CAP1_E1CHUNK_SHFT)
#define CAP1_E1GF    (0x1fU << CAP1_E1GF_SHFT)
#define CAP1_E0CAP   (0x3fU << CAP1_E0CAP_SHFT)
#define CAP1_E0CHUNK (0x1fU << CAP1_E0CHUNK_SHFT)
#define CAP1_E0GF    (0x1fU << CAP1_E0GF_SHFT)


/* Capability 2/3/4 register */
#define CAP_MSEL_SHFT   31
#define CAP_MSPARE_SHFT 16

#define CAP_MSEL   (1U << CAP_MSEL_SHFT)
#define CAP_MSPARE (0x1fffU << CAP_MSPARE_SHFT)
#define CAP_MDATA  0xffffU


/* ======================== Type Definitions ======================== */


typedef struct {
	u32 ctrl0;    /* 0x000: Core control 0 register */
	u32 ctrl1;    /* 0x004: Core control 1 register */
	u32 ctrl2;    /* 0x008: Core control 2 register */
	u32 ctrl3;    /* 0x00c: Core control 3 register */
	u32 ctrl4;    /* 0x010: Core control 4 register */
	u32 res0[3];  /* 0x014 - 0x01f: Reserved */
	u32 sts0;     /* 0x020: Core status 0 register */
	u32 sts1;     /* 0x024: Core status 1 register */
	u32 sts2;     /* 0x028: Core status 2 register */
	u32 sts3;     /* 0x02c: Core status 3 register */
	u32 res1[2];  /* 0x030 - 0x037: Reserved */
	u32 cap0;     /* 0x038: Capability 0 register */
	u32 cap1;     /* 0x03c: Capability 1 register */
	u32 cap2;     /* 0x040: Capability 2 register */
	u32 cap3;     /* 0x044: Capability 3 register */
	u32 cap4;     /* 0x048: Capability 4 register */
	u32 cap5;     /* 0x04c: Capability 5 register */
	u32 tme0;     /* 0x050: Programmable timing 0 register */
	u32 tme1;     /* 0x054: Programmable timing 1 register */
	u32 tme2;     /* 0x058: Programmable timing 2 register */
	u32 tme3;     /* 0x05c: Programmable timing 3 register */
	u32 tme4;     /* 0x060: Programmable timing 4 register */
	u32 tme5;     /* 0x064: Programmable timing 5 register */
	u32 tme6;     /* 0x068: Programmable timing 6 register */
	u32 tme7;     /* 0x06c: Programmable timing 7 register */
	u32 tme8;     /* 0x070: Programmable timing 8 register */
	u32 tme9;     /* 0x074: Programmable timing 9 register */
	u32 tme10;    /* 0x078: Programmable timing 10 register */
	u32 tme11;    /* 0x07c: Programmable timing 11 register */
	u32 res2[16]; /* 0x080 - 0x0bf: Reserved */
	u32 txskew0;  /* 0x0c0: Skew Control Transfer register */
	u32 rxskew0;  /* 0x0c4: Skew Control Receive register */
	u32 res3[2];  /* 0x0c8 - 0x0cf: Reserved */
	u32 tout0;    /* 0x0d0: Programmable timeout 0 register */
	u32 res4[31]; /* 0x0d4 - 0x14f: Reserved */
	u32 llpl;     /* 0x150: Linked list pointer low register */
	u32 res5;     /* 0x154: Reserved */
	u32 res6[2];  /* 0x158 - 0x15f: Reserved */
	u32 dcmd;     /* 0x160: Descriptor command register */
	u32 dtarsel0; /* 0x164: Descriptor target select 0 register */
	u32 dtarsel1; /* 0x168: Descriptor target select 1 register */
	u32 dchsel;   /* 0x16c: Descriptor channel select register */
	u32 drbsel;   /* 0x170: Descriptor ready/busy select register */
	u32 drow;     /* 0x174: Descriptor row & control register */
	u32 dcolsize; /* 0x178: Descriptor column & size register */
	u32 dsts;     /* 0x17c: Descriptor status register */
	u32 deccsts0; /* 0x180: Descriptor ECC status register */
	u32 res7[3];  /* 0x184 - 0x18f: Reserved */
	u32 ddpl;     /* 0x190: Descriptor data pointer low register */
	u32 res8[19]; /* 0x194 - 0x1df: Reserved */
	u32 din0;     /* 0x1e0: Data-in 0 register */
	u32 din1;     /* 0x1e4: Data-in 1 register */
	u32 din2;     /* 0x1e8: Data-in 2 register */
	u32 din3;     /* 0x1ec: Data-in 3 register */
	u32 dout0;    /* 0x1f0: Data-out 0 register */
	u32 dout1;    /* 0x1f4: Data-out 1 register */
	u32 dout2;    /* 0x1f8: Data-out 2 register */
	u32 dout3;    /* 0x1fc: Data-out 3 register */
} nandfctrl2_regs_t;


typedef struct {
	u64 ceMask;
	u32 rbMask;
	u32 channelMask;
} nand_layoutMap_t;


typedef struct {
	u32 cmd;
	u32 rowAddr; /* 24-bit Page/Block address */
	u32 colAddr; /* 16-bit Byte offset within the page */

	u8 skipAddr; /* SA bit */
	u8 disEcc;   /* ED bit */

	size_t transferSize; /* Number of bytes to read/write */
	addr_t data;         /* RAM pointer for DMA (NULL if no data phase) */
} nand_op_t;


typedef struct {
	u8 manufacturerId;
	u8 deviceId;
	u8 bytes[3];
} __attribute__((packed)) flash_id_t;


typedef struct {
	u8 cmd1;
	u8 cmd2;
	// u8 addrsz;
	u8 skipCmd2;
} nand_cmd_t;


static const nand_cmd_t nand_commands[cmd_count] = {
	{ 0x00U, 0x30U, 0U }, /* cmd_read */
	{ 0x00U, 0x32U, 0U }, /* cmd_read_multiplane */
	{ 0x00U, 0x35U, 0U }, /* cmd_copyback_read */
	{ 0x05U, 0xe0U, 0U }, /* cmd_change_read_column */
	{ 0x06U, 0xe0U, 0U }, /* cmd_change_read_column_enhanced */
	{ 0x00U, 0x31U, 0U }, /* cmd_read_cache_random */
	{ 0x31U, 0x00U, 1U }, /* cmd_read_cache_sequential */
	{ 0x3fU, 0x00U, 1U }, /* cmd_read_cache_end */
	{ 0x60U, 0xd0U, 0U }, /* cmd_block_erase */
	{ 0x60U, 0xd1U, 0U }, /* cmd_block_erase_multiplane */
	{ 0x70U, 0x00U, 1U }, /* cmd_read_status */
	{ 0x78U, 0x00U, 1U }, /* cmd_read_status_enhanced */
	{ 0x80U, 0x10U, 0U }, /* cmd_page_program */
	{ 0x80U, 0x11U, 0U }, /* cmd_page_program_multiplane */
	{ 0x80U, 0x15U, 0U }, /* cmd_page_cache_program */
	{ 0x85U, 0x10U, 0U }, /* cmd_copyback_program */
	{ 0x85U, 0x11U, 0U }, /* cmd_copyback_program_multiplane */
	{ 0x85U, 0x11U, 0U }, /* cmd_small_data_move */
	{ 0x85U, 0x00U, 1U }, /* cmd_change_write_column */
	{ 0x85U, 0x00U, 1U }, /* cmd_change_row_address */
	{ 0x90U, 0x00U, 1U }, /* cmd_read_id */
	{ 0xe1U, 0x00U, 1U }, /* cmd_volume_select */
	{ 0xe2U, 0x00U, 1U }, /* cmd_odt_configure */
	{ 0xecU, 0x00U, 1U }, /* cmd_read_parameter_page */
	{ 0xedU, 0x00U, 1U }, /* cmd_read_unique_id */
	{ 0xeeU, 0x00U, 1U }, /* cmd_get_features */
	{ 0xefU, 0x00U, 1U }, /* cmd_set_features */
	{ 0xd4U, 0x00U, 1U }, /* cmd_lun_get_features */
	{ 0xd5U, 0x00U, 1U }, /* cmd_lun_set_features */
	{ 0xd9U, 0x00U, 1U }, /* cmd_zq_calibration_short */
	{ 0xf9U, 0x00U, 1U }, /* cmd_zq_calibration_long */
	{ 0xfaU, 0x00U, 1U }, /* cmd_reset_lun */
	{ 0xfcU, 0x00U, 1U }, /* cmd_synchronous_reset */
	{ 0xffU, 0x00U, 1U }  /* cmd_reset */
};


static const nand_layoutMap_t nand_flashMap[NAND_DIE_CNT] = NAND_DIE_MAP;


static struct {
	volatile nandfctrl2_regs_t *regs;

	u8 initialized;

	u8 dmaDataBuf[16 * 1024] __attribute__((aligned(64)));
} nand_common;


/* ======================== Internal Helpers ======================== */


static u32 getRowAddr(const nand_die_t *nand, u32 block, u16 page)
{
	return (block * nand->info.pagesPerBlock) + page;
}


static void setupRouting(const nand_die_t *nand)
{
	nand_common.regs->dtarsel0 = (u32)(nand_flashMap[nand->minor].ceMask & 0xffffffffU);
	nand_common.regs->dtarsel1 = (u32)(nand_flashMap[nand->minor].ceMask >> 32);
	nand_common.regs->drbsel = nand_flashMap[nand->minor].rbMask;
	nand_common.regs->dchsel = nand_flashMap[nand->minor].channelMask;
}


static void wait4Nandfctrl2Ready(void)
{
	while ((nand_common.regs->sts0 & STS0_RDY) == 0) { }
}


static void wait4DescriptorDone(void)
{
	while ((nand_common.regs->sts0 & STS0_DA) != 0) { }
}


static u32 getNandfctrl2Status(void)
{
	u32 status = nand_common.regs->sts1;

	/* Clear status */
	nand_common.regs->sts1 = status;

	return status;
}


static void abortOp(void)
{
	nand_common.regs->ctrl2 = CTRL2_ABORT;

	while (((nand_common.regs->sts1 & STS1_ABORT) == 0) || ((nand_common.regs->sts0 & STS0_RDY) == 0)) { }

	/* Clear error status */
	(void)getNandfctrl2Status();
}


static int executeOp(const nand_die_t *nand, const nand_op_t *op)
{
	const nand_cmd_t *cmd;

	if (op->cmd >= cmd_count) {
		return -EINVAL;
	}

	cmd = &nand_commands[op->cmd];

	wait4Nandfctrl2Ready();

	setupRouting(nand);

	if (op->data != 0U) {
		nand_common.regs->ddpl = (u32)op->data;
	}

	/* Set Address and Size */
	nand_common.regs->drow = op->rowAddr;
	nand_common.regs->dcolsize = (op->transferSize << DCOLSIZE_SIZE_SHFT) | op->colAddr;

	/* Build the DCMD register value */
	u32 dcmd = 0;
	dcmd |= (cmd->cmd1 << DCMD_CMD1_SHFT);
	dcmd |= (cmd->cmd2 << DCMD_CMD2_SHFT);
	dcmd |= DCMD_EN;

	if (cmd->skipCmd2) {
		dcmd |= DCMD_SC2;
	}
	if (op->skipAddr) {
		dcmd |= DCMD_SA;
	}
	if (op->disEcc) {
		dcmd |= DCMD_ED;
	}
	if (op->data == 0U) {
		dcmd |= DCMD_SD;
	}

	nand_common.regs->dcmd = dcmd;

	/* Trigger the execution */
	nand_common.regs->ctrl2 = CTRL2_DT;

	wait4DescriptorDone();

	u32 hwStatus = getNandfctrl2Status();

	if ((hwStatus & (STS1_TMOUT | STS1_CMD | STS1_ABORT)) != 0U) {
		abortOp();
		return -ETIME;
	}

	return EOK;
}


static int readFlashStatus(const nand_die_t *nand)
{
	int err;
	nand_op_t op = {
		.cmd = cmd_read_status,
		.rowAddr = 0U,
		.colAddr = 0U,
		.transferSize = 1U,
		.data = (addr_t)nand_common.dmaDataBuf,
		.skipAddr = 1U,
		.disEcc = 1U,
	};

	err = executeOp(nand, &op);
	if (err < 0) {
		return err;
	}

	return (int)nand_common.dmaDataBuf[0];
}


static int executeOpCheckFlashSts(const nand_die_t *nand, const nand_op_t *op)
{
	int status = executeOp(nand, op);
	if (status < 0) {
		return status;
	}

	status = readFlashStatus(nand);
	if (status < 0) {
		return status;
	}

	return (status & 0x01U) ? -EIO : 0;
}


static int readFlashId(const nand_die_t *nand, flash_id_t *flashId)
{
	nand_op_t op;

	op.cmd = cmd_read_id;
	op.rowAddr = 0U;
	op.colAddr = 0U;
	op.transferSize = sizeof(*flashId);
	op.data = (addr_t)flashId;
	op.skipAddr = 0U;
	op.disEcc = 1U;

	return executeOp(nand, &op);
}


static int readOnfiParameterPage(const nand_die_t *nand, u8 *rawBuf, size_t size)
{
	nand_op_t op;

	op.cmd = cmd_read_parameter_page;
	op.rowAddr = 0U;
	op.colAddr = 0U;
	op.transferSize = size;
	op.data = (addr_t)rawBuf;
	op.skipAddr = 0U;
	op.disEcc = 1U;

	return executeOp(nand, &op);
}


static u32 ns2cycles(u32 ns, u32 coreClk)
{
	u32 cycles = (u32)(((u64)ns * coreClk + 999999999U) / 1000000000U);
	return (cycles > 0U) ? cycles : 1U;
}


static void setOnfiTimingMode(unsigned int mode, const onfi_paramPage_t *paramPage, u32 coreClk, int edoEn)
{
	u32 tCS, tWW, tRR, tWB, tRHW, tWHR, tCCS, tADL, tREH, tRP, tRC, tWH, tWP, tWC, tVDLY;
	const onfi_timingMode_t *timings = onfi_getTimingModeSDR(mode);

	/* Most timings are programmed as (number of cycles - 1) */
	tCS = (ns2cycles(timings->tCS3, coreClk) - 1U) & 0xffffU;
	tWW = (ns2cycles(timings->tWW, coreClk) - 1U) & 0xffffU;

	nand_common.regs->tme0 = (tCS << 16) | tWW;

	tRR = (ns2cycles(timings->tRR, coreClk) - 1U) & 0xffffU;
	tWB = (ns2cycles(timings->tWB, coreClk) - 1U) & 0xffffU;

	nand_common.regs->tme1 = (tRR << 16) | tWB;

	tRHW = (ns2cycles(timings->tRHW, coreClk) - 1U) & 0xffffU;
	tWHR = (ns2cycles(timings->tWHR, coreClk) - 1U) & 0xffffU;

	nand_common.regs->tme2 = (tRHW << 16) | tWHR;

	if (paramPage == NULL) {
		tCCS = (ns2cycles(ONFI_TCCS_BASE, coreClk) - 1U) & 0xffffU;
		tADL = (ns2cycles(timings->tADL, coreClk) - 1U) & 0xffffU;
	}
	else {
		tCCS = (ns2cycles(paramPage->tCCS, coreClk) - 1U) & 0xffffU;
		tADL = (ns2cycles(paramPage->tADL, coreClk) - 1U) & 0xffffU;
	}

	nand_common.regs->tme3 = (tADL << 16) | tCCS;

	tREH = ns2cycles(timings->tREH, coreClk) & 0xffffU;
	tRP = (edoEn != 0) ? (ns2cycles(timings->tRP, coreClk) & 0xffffU) : (ns2cycles(timings->tREA, coreClk) & 0xffffU);
	tRC = ns2cycles(timings->tRC, coreClk);

	/* Ensure sum meets tRC */
	if ((tREH + tRP) < tRC) {
		tREH += (tRC - (tREH + tRP));
	}

	nand_common.regs->tme4 = ((tREH - 1U) << 16) | (tRP - 1U);

	tWH = ns2cycles(timings->tWH, coreClk) & 0xffffU;
	tWP = ns2cycles(timings->tWP, coreClk) & 0xffffU;
	tWC = ns2cycles(timings->tWC, coreClk);

	/* Ensure sum meets tWC */
	if ((tWH + tWP) < tWC) {
		tWH += (tWC - (tWH + tWP));
	}

	nand_common.regs->tme5 = ((tWH - 1U) << 16) | (tWP - 1U);

	/* tme6-tme10 not used in SDR mode */
	nand_common.regs->tme6 = 0;
	nand_common.regs->tme7 = 0;
	nand_common.regs->tme8 = 0;
	nand_common.regs->tme9 = 0;
	nand_common.regs->tme10 = 0;

	tVDLY = (ns2cycles(ONFI_TVDLY, coreClk) - 1U) & 0xffffU;

	nand_common.regs->tme11 = (tCS << 16) | tVDLY;

	if (edoEn != 0) {
		nand_common.regs->ctrl0 |= CTRL0_EDO;
	}
	else {
		nand_common.regs->ctrl0 &= ~CTRL0_EDO;
	}
}


static void resetNandfctrl2(void)
{
	abortOp();

	nand_common.regs->ctrl2 = CTRL2_RST;

	nand_common.regs->ctrl0 = CTRL0_BBM | CTRL0_EE;
	/* Disable all interrupts */
	nand_common.regs->ctrl1 = 0U;
	/* Disable write protection */
	nand_common.regs->ctrl3 = 0U;
	/* Set SEFI low */
	nand_common.regs->ctrl4 = 0U;

	setOnfiTimingMode(0, NULL, SYSCLK_FREQ, 0);

	/* Disable timeout */
	nand_common.regs->tout0 = 0U;
}


static int setMemorySelect(u32 pageSz, u16 spareSz)
{
	const u32 cap2 = nand_common.regs->cap2;
	const u32 cap3 = nand_common.regs->cap3;
	const u32 cap4 = nand_common.regs->cap4;

	u32 msel;

	if (((cap2 & CAP_MDATA) == pageSz) && (((cap2 & CAP_MSPARE) >> CAP_MSPARE_SHFT) <= spareSz)) {
		msel = 0U;
	}
	else if (((cap3 & CAP_MDATA) == pageSz) && (((cap3 & CAP_MSPARE) >> CAP_MSPARE_SHFT) <= spareSz)) {
		msel = 1U;
	}
	else if (((cap4 & CAP_MDATA) == pageSz) && (((cap4 & CAP_MSPARE) >> CAP_MSPARE_SHFT) <= spareSz)) {
		msel = 2U;
	}
	else {
		return -EINVAL;
	}

	nand_common.regs->ctrl0 |= (msel << CTRL0_MSEL_SHFT);

	return 0;
}


static void fillSpareLayout(nandfctrl2_info_t *info)
{
	const u32 msel = (nand_common.regs->ctrl0 & CTRL0_MSEL_MSK) >> CTRL0_MSEL_SHFT;
	const volatile u32 *cap = &nand_common.regs->cap2 + msel;
	const u32 eccSel = (*cap & CAP_MSEL) >> CAP_MSEL_SHFT;
	const u32 mspare = (*cap & CAP_MSPARE) >> CAP_MSPARE_SHFT;
	const u32 cap1 = nand_common.regs->cap1;

	u32 chunksz, gfsz, eccCap;

	if (eccSel == 0) {
		chunksz = (cap1 & CAP1_E0CHUNK) >> CAP1_E0CHUNK_SHFT;
		gfsz = (cap1 & CAP1_E0GF) >> CAP1_E0GF_SHFT;
		eccCap = (cap1 & CAP1_E0CAP) >> CAP1_E0CAP_SHFT;
	}
	else {
		chunksz = (cap1 & CAP1_E1CHUNK) >> CAP1_E1CHUNK_SHFT;
		gfsz = (cap1 & CAP1_E1GF) >> CAP1_E1GF_SHFT;
		eccCap = (cap1 & CAP1_E1CAP) >> CAP1_E1CAP_SHFT;
	}

	info->eccChunksz = 1U << chunksz;
	info->eccsz = 2 * ((eccCap * gfsz + 15U) / 16U) * (info->writesz / info->eccChunksz);
	info->eccCap = eccCap;

	info->sparesz = mspare;
	info->spareavail = info->sparesz - info->eccsz - NAND_BBM_SIZE;
}


static int setupDie(nand_die_t *nand)
{
	int edoEn, err, timingMode;
	onfi_paramPage_t paramPage;
	flash_id_t *flashId = (flash_id_t *)nand_common.dmaDataBuf;

	hal_memset(flashId, 0, sizeof(*flashId));

	err = readFlashId(nand, flashId);
	if (err != 0) {
		lib_printf("\nnandfctrl2/flash%u: couldn't read flash ID", nand->minor);
		return err;
	}

	if ((flashId->manufacturerId == 0x2cU) && (flashId->deviceId == 0xacU)) {
		nand->info.name = "Micron MT29F4G08ABBFA";
	}
	else {
		nand->info.name = "Unknown NAND flash";
	}

	hal_memset(nand_common.dmaDataBuf, 0, sizeof(onfi_paramPage_t));

	if (readOnfiParameterPage(nand, nand_common.dmaDataBuf, sizeof(onfi_paramPage_t)) < 0) {
		lib_printf("\nnandfctrl2/flash%u: couldn't read ONFI parameter page", nand->minor);
		return -EIO;
	}

	onfi_deserializeParamPage(&paramPage, nand_common.dmaDataBuf);

	if ((paramPage.signature[0] != 'O') || (paramPage.signature[1] != 'N') || (paramPage.signature[2] != 'F') || (paramPage.signature[3] != 'I')) {
		lib_printf("\nnandfctrl2/flash%u: couldn't read ONFI parameter page", nand->minor);
		return -EIO;
	}

	timingMode = onfi_calcTimingMode(&paramPage);

	edoEn = (timingMode > 3) ? 1 : 0;

	setOnfiTimingMode(timingMode, &paramPage, SYSCLK_FREQ, edoEn);

	if (setMemorySelect(paramPage.bytesPerPage, paramPage.spareBytesPerPage) != 0) {
		lib_printf("\nnandfctrl2/flash%u: unsupported config (pageSz: %u, spareSz: %u)", nand->minor, paramPage.bytesPerPage, paramPage.spareBytesPerPage);
		return -EINVAL;
	}

	nand->info.erasesz = paramPage.bytesPerPage * paramPage.pagesPerBlock;
	nand->info.size = nand->info.erasesz * paramPage.blocksPerLun * paramPage.numLuns;
	nand->info.writesz = paramPage.bytesPerPage;
	nand->info.pagesPerBlock = paramPage.pagesPerBlock;
	fillSpareLayout(&nand->info);

	return 0;
}


static int isBlockBadPhys(const nand_die_t *nand, u32 block)
{
	const u32 pagesz = nand->info.writesz;
	u8 *buf = nand_common.dmaDataBuf;
	nand_op_t op;
	int status;

	op.cmd = cmd_read;
	op.rowAddr = getRowAddr(nand, block, 0U);
	op.colAddr = pagesz;
	op.transferSize = 1U;
	op.data = (addr_t)buf;
	op.skipAddr = 0U;
	op.disEcc = 1U;

	status = executeOp(nand, &op);
	if (status < 0) {
		return 1;
	}

	return (buf[0] == 0x00U) ? 1 : 0;
}


static void badBlockTableSet(nand_die_t *nand, u32 block)
{
	nand->bbt[block >> 5] |= (1U << (block & 31));
}


static int scanBadBlocks(nand_die_t *nand)
{
	u32 block;
	const u32 numBlocks = nand->info.size / nand->info.erasesz;

	if (numBlocks > NANDFCTRL2_BBT_MAX_BLOCKS) {
		lib_printf("nandfctrl2/flash%u: unsupported config: %u blocks\n", nand->minor, numBlocks);
		return -EINVAL;
	}

	for (block = 0; block < numBlocks; block++) {
		if (isBlockBadPhys(nand, block)) {
			badBlockTableSet(nand, block);
		}
	}

	return 0;
}


/* ======================== Public API ======================== */


nand_die_t *nand_get(u32 minor)
{
	static nand_die_t dev[NAND_DIE_CNT];

	if (minor < NAND_DIE_CNT) {
		return &dev[minor];
	}
	return NULL;
}


int nandfctrl2_resetFlash(const nand_die_t *nand)
{
	nand_op_t op = {
		.cmd = cmd_reset,
		.rowAddr = 0U,
		.colAddr = 0U,
		.transferSize = 0U,
		.data = 0U,
		.skipAddr = 1U,
		.disEcc = 1U,
	};

	return executeOp(nand, &op);
}


int nandfctrl2_pageWrite(const nand_die_t *nand, u32 page, const void *data)
{
	nand_op_t op = {
		.cmd = cmd_page_program,
		.rowAddr = page,
		.skipAddr = 0U,
		.disEcc = 0U,
		.colAddr = 0U,
		.transferSize = nand->info.writesz,
		.data = (addr_t)data,
	};

	return executeOpCheckFlashSts(nand, &op);
}


int nandfctrl2_pageRead(const nand_die_t *nand, u32 page, void *data)
{
	nand_op_t op = {
		.cmd = cmd_read,
		.rowAddr = page,
		.skipAddr = 0U,
		.disEcc = 0U,
		.colAddr = 0U,
		.transferSize = nand->info.writesz,
		.data = (addr_t)data,
	};

	return executeOp(nand, &op);
}


int flashdrv_metaWrite(const nand_die_t *nand, u32 page, const void *data, size_t size)
{
	u32 tagOfs = nand->info.writesz + nand->info.sparesz - nand->info.spareavail;
	nand_op_t op = {
		.cmd = cmd_page_program,
		.rowAddr = page,
		.colAddr = tagOfs,
		.transferSize = size,
		.data = (addr_t)data,
		.skipAddr = 0U,
		.disEcc = 1U,
	};

	return executeOpCheckFlashSts(nand, &op);
}


int flashdrv_metaRead(const nand_die_t *nand, u32 page, void *data, size_t size)
{
	u32 tagOfs = nand->info.writesz + nand->info.sparesz - nand->info.spareavail;
	nand_op_t op = {
		.cmd = cmd_read,
		.rowAddr = page,
		.colAddr = tagOfs,
		.transferSize = size,
		.data = (addr_t)data,
		.skipAddr = 0U,
		.disEcc = 1U,
	};

	return executeOp(nand, &op);
}


int nandfctrl2_eraseBlock(const nand_die_t *nand, u32 block)
{
	nand_op_t op = {
		.cmd = cmd_block_erase,
		.rowAddr = getRowAddr(nand, block, 0U),
		.colAddr = 0U,
		.transferSize = 0U,
		.data = 0U,
		.skipAddr = 0U,
		.disEcc = 0U,
	};

	return executeOpCheckFlashSts(nand, &op);
}


int nandfctrl2_isbad(const nand_die_t *nand, u32 block)
{
	return (nand->bbt[block >> 5] >> (block & 31)) & 1U;
}


int nandfctrl2_markbad(nand_die_t *nand, u32 block)
{
	const u32 pagesz = nand->info.writesz;
	u8 *buf = nand_common.dmaDataBuf;
	nand_op_t op;
	int err;

	buf[0] = 0x00U;

	op.cmd = cmd_page_program;
	op.rowAddr = getRowAddr(nand, block, 0U);
	op.colAddr = pagesz;
	op.transferSize = 1U;
	op.data = (addr_t)buf;
	op.skipAddr = 0U;
	op.disEcc = 1U;

	err = executeOpCheckFlashSts(nand, &op);
	if (err == 0) {
		badBlockTableSet(nand, block);
	}

	return err;
}


int nandfctrl2_init(nand_die_t *nand)
{
	int err;

	if (nand_common.initialized == 0U) {

		nand_common.regs = (nandfctrl2_regs_t *)NANDFCTRL2_BASE;

		resetNandfctrl2();

		nand_common.initialized = 1U;
	}

	nandfctrl2_resetFlash(nand);

	err = setupDie(nand);
	if (err < 0) {
		return err;
	}

	err = nandfctrl2_resetFlash(nand);
	if (err < 0) {
		return err;
	}

	err = scanBadBlocks(nand);

	return err;
}
