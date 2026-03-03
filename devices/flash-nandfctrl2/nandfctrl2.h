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

#ifndef FLASH_NANDFCTRL2_DRV_H_
#define FLASH_NANDFCTRL2_DRV_H_


#include <types.h>


#define NAND_MAX_BLOCK_SIZE       (2U * 1024 * 1024) /* Up to 2MB eraseblock */
#define NANDFCTRL2_BBT_MAX_BLOCKS 4096               /* Maximum blocks per die for the Bad Block Table */


/* clang-format off */
enum {
	cmd_read = 0, cmd_read_multiplane, cmd_copyback_read, cmd_change_read_column, cmd_change_read_column_enhanced,
	cmd_read_cache_random, cmd_read_cache_sequential, cmd_read_cache_end, cmd_block_erase, cmd_block_erase_multiplane,
	cmd_read_status, cmd_read_status_enhanced, cmd_page_program, cmd_page_program_multiplane, cmd_page_cache_program,
	cmd_copyback_program, cmd_copyback_program_multiplane, cmd_small_data_move, cmd_change_write_column,
	cmd_change_row_address, cmd_read_id, cmd_volume_select, cmd_odt_configure, cmd_read_parameter_page,
	cmd_read_unique_id, cmd_get_features, cmd_set_features, cmd_lun_get_features, cmd_lun_set_features,
	cmd_zq_calibration_short, cmd_zq_calibration_long, cmd_reset_lun, cmd_synchronous_reset, cmd_reset,
	cmd_count
};
/* clang-format on */


/* NAND flash configuration */
typedef struct {
	const char *name;  /* Flash device name string */
	u64 size;          /* Total NAND size in bytes */
	u32 writesz;       /* Page data size in bytes */
	u32 sparesz;       /* Total spare (OOB) bytes per page */
	u32 spareavail;    /* User-accessible spare bytes */
	u32 erasesz;       /* Erase block size in bytes */
	u32 pagesPerBlock; /* Pages per erase block */
	u32 eccChunksz;    /* ECC chunk size in bytes */
	u32 eccsz;         /* ECC bytes per chunk */
	u32 eccCap;        /* ECC correction capability in bits */
} nandfctrl2_info_t;


typedef struct {
	u32 minor;
	nandfctrl2_info_t info;

	/* Block cache */
	u32 blockBufNum; /* Cached eraseblock index */
	u32 blockBufDirty;
	u8 blockBuf[NAND_MAX_BLOCK_SIZE] __attribute__((aligned(8)));

	/* Bad Block Table */
	u32 bbt[NANDFCTRL2_BBT_MAX_BLOCKS / 32]; /* BBT bitmap */
} nand_die_t;


nand_die_t *nand_get(u32 minor);


int nandfctrl2_resetFlash(const nand_die_t *nand);


int nandfctrl2_pageWrite(const nand_die_t *nand, u32 page, const void *data);


int nandfctrl2_pageRead(const nand_die_t *nand, u32 page, void *data);


int flashdrv_metaWrite(const nand_die_t *nand, u32 page, const void *metadata, size_t size);


int flashdrv_metaRead(const nand_die_t *nand, u32 page, void *metadata, size_t size);


int nandfctrl2_eraseBlock(const nand_die_t *nand, u32 block);


int nandfctrl2_isbad(const nand_die_t *nand, u32 block);


int nandfctrl2_markbad(nand_die_t *nand, u32 block);


int nandfctrl2_init(nand_die_t *nand);


#endif
