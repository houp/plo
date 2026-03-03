/*
 * Phoenix-RTOS
 *
 * Operating system loader
 *
 * GRLIB NANDFCTRL2 Data Interface
 *
 * Copyright 2026 Phoenix Systems
 * Author: Lukasz Leczkowski
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <devices/devs.h>
#include <hal/hal.h>
#include <lib/lib.h>

#include "data.h"
#include "nandfctrl2.h"


#define NAND_ERASED_STATE 0xffU


static void invalidateBlockBuf(nand_die_t *nand)
{
	nand->blockBufDirty = 0;
	nand->blockBufNum = (u32)-1;
}


static int cacheBlock(nand_die_t *nand, u32 block)
{
	u32 page, rpage;
	int err;

	for (page = 0U; page < nand->info.pagesPerBlock; page++) {
		rpage = block * nand->info.pagesPerBlock + page;
		err = nandfctrl2_pageRead(nand, rpage, nand->blockBuf + (page * nand->info.writesz));
		if (err < 0) {
			if (err == -ETIME) {
				(void)nandfctrl2_resetFlash(nand);
			}
			return err;
		}
	}

	nand->blockBufNum = block;

	return EOK;
}


int data_doSync(nand_die_t *nand)
{
	const u32 blockcnt = nand->info.size / nand->info.erasesz;
	addr_t pos;
	u32 wpage;
	int err;

	if ((nand->blockBufNum == (u32)-1) || (nand->blockBufDirty == 0)) {
		return EOK;
	}

	do {
		/* The cached block might have become a bad block
		 * Find first good block to write the cached data */
		while ((nand->blockBufNum < blockcnt) && nandfctrl2_isbad(nand, nand->blockBufNum)) {
			nand->blockBufNum++;
		}

		/* Fatal error, no good block available */
		if (nand->blockBufNum >= blockcnt) {
			return -ENOSPC;
		}

		/* Erase and write the block */
		err = nandfctrl2_eraseBlock(nand, nand->blockBufNum);
		if (err == -ETIME) {
			(void)nandfctrl2_resetFlash(nand);
			return err;
		}

		for (pos = 0; (pos < nand->info.erasesz) && (err >= 0); pos += nand->info.writesz) {
			wpage = nand->blockBufNum * nand->info.pagesPerBlock + pos / nand->info.writesz;
			err = nandfctrl2_pageWrite(nand, wpage, nand->blockBuf + pos);
			if (err == -ETIME) {
				(void)nandfctrl2_resetFlash(nand);
				return err;
			}
		}

		if (err < 0) {
			/* Flash reported operation failure, mark block as bad and try to sync on the next block */
			if (nandfctrl2_markbad(nand, nand->blockBufNum) < 0) {
				/* Fatal error, can't recover */
				return -EIO;
			}
			nand->blockBufNum++;
		}
	} while (err < 0);

	nand->blockBufDirty = 0;

	return EOK;
}


static int data_sync(unsigned int minor)
{
	nand_die_t *nand = nand_get(minor);

	if (nand == NULL) {
		return -ENODEV;
	}

	return data_doSync(nand);
}


static ssize_t data_read(unsigned int minor, addr_t offs, void *buff, size_t len, time_t timeout)
{
	nand_die_t *nand = nand_get(minor);
	u32 block, blockcnt;
	addr_t boffs;
	size_t size, ret = 0;

	(void)timeout;

	if (nand == NULL) {
		return -ENODEV;
	}

	if (offs >= nand->info.size) {
		return -EINVAL;
	}

	len = min(len, nand->info.size - offs);

	if (len == 0U) {
		return 0;
	}

	boffs = offs % nand->info.erasesz;
	blockcnt = nand->info.size / nand->info.erasesz;

	/* Read block by block. */
	for (block = offs / nand->info.erasesz; (block < blockcnt) && (ret < len); block++) {
		/* Skip bad blocks */
		if (nandfctrl2_isbad(nand, block) != 0) {
			continue;
		}

		if (block != nand->blockBufNum) {
			int err = cacheBlock(nand, block);
			if (err < 0) {
				if (err != -ETIME) {
					/* Block read failed (block data is lost), mark it as bad and exit */
					invalidateBlockBuf(nand);

					(void)nandfctrl2_markbad(nand, block);
				}

				return err;
			}
		}

		size = min(len - ret, nand->info.erasesz - boffs);
		(void)hal_memcpy((u8 *)buff + ret, nand->blockBuf + boffs, size);
		ret += size;

		boffs = 0;
	}

	return min(ret, len);
}


static ssize_t data_write(unsigned int minor, addr_t offs, const void *buff, size_t len)
{
	nand_die_t *nand = nand_get(minor);
	u32 block, blockcnt, cblock, blockBasePage, page, rpage;
	addr_t boffs;
	size_t size, ret = 0;
	int err = 0;

	if (nand == NULL) {
		return -ENODEV;
	}

	if (offs >= nand->info.size) {
		return -EINVAL;
	}

	len = min(len, nand->info.size - offs);

	if (len == 0U) {
		return 0;
	}

	blockcnt = nand->info.size / nand->info.erasesz;
	boffs = offs % nand->info.erasesz;

	for (block = offs / nand->info.erasesz; (block < blockcnt) && (ret < len); block++) {
		/* Skip bad blocks */
		if (nandfctrl2_isbad(nand, block) != 0) {
			continue;
		}

		if (block != nand->blockBufNum) {
			/* Block to write not in cache */
			cblock = nand->blockBufNum;

			err = data_doSync(nand);
			if (err < 0) {
				/* Fatal error, can't sync cached block */
				return err;
			}

			/* Cached data got synced at our block, find next good block */
			if ((block > cblock) && (block <= nand->blockBufNum)) {
				block = nand->blockBufNum;
				continue;
			}

			invalidateBlockBuf(nand);

			blockBasePage = block * nand->info.pagesPerBlock;

			/* Read new cached block data before the written area */
			for (page = 0; (page * nand->info.writesz) < boffs; page++) {
				rpage = blockBasePage + page;
				err = nandfctrl2_pageRead(nand, rpage, nand->blockBuf + (page * nand->info.writesz));
				if (err == -ETIME) {
					(void)nandfctrl2_resetFlash(nand);
					return err;
				}
				if (err < 0) {
					break;
				}
			}

			if (err >= 0) {
				/* Read new cached block data after the written area */
				for (page = max(page, (boffs + len - ret) / nand->info.writesz); page < nand->info.pagesPerBlock; page++) {
					rpage = blockBasePage + page;
					err = nandfctrl2_pageRead(nand, rpage, nand->blockBuf + (page * nand->info.writesz));
					if (err == -ETIME) {
						(void)nandfctrl2_resetFlash(nand);
						/* Let user retry */
						return err;
					}
					if (err < 0) {
						break;
					}
				}
			}

			if (err < 0) {
				/* Block read failed (block data is lost), mark it as bad and find next good block */
				if (nandfctrl2_markbad(nand, block) < 0) {
					return -EIO;
				}
				continue;
			}

			nand->blockBufNum = block;
		}

		/* Copy data to cache */
		size = min(len - ret, nand->info.erasesz - boffs);
		(void)hal_memcpy(nand->blockBuf + boffs, (const u8 *)buff + ret, size);
		ret += size;

		nand->blockBufDirty = 1;

		boffs = 0;
	}

	return min(ret, len);
}


static ssize_t data_erase(unsigned int minor, addr_t offs, size_t len, unsigned int flags)
{
	nand_die_t *nand = nand_get(minor);
	u32 boffs, size, block, blockcnt, blockBasePage, page, rpage;
	size_t ret = 0;
	int err;

	(void)flags;

	if (nand == NULL) {
		return -ENODEV;
	}

	if (offs >= nand->info.size) {
		return -EINVAL;
	}

	len = min(len, nand->info.size - offs);
	if (len == 0U) {
		return 0;
	}

	blockcnt = nand->info.size / nand->info.erasesz;
	boffs = offs % nand->info.erasesz;

	for (block = offs / nand->info.erasesz; (block < blockcnt) && (ret < len); block++) {
		/* Skip bad blocks */
		if (nandfctrl2_isbad(nand, block) != 0) {
			continue;
		}

		if ((boffs != 0U) || ((ret + nand->info.erasesz) > len)) {
			/* Partial block erase */

			/* Check if the block is in the cache. */
			if (nand->blockBufNum != block) {
				err = data_doSync(nand);
				if (err < 0) {
					return err;
				}

				invalidateBlockBuf(nand);

				blockBasePage = block * nand->info.pagesPerBlock;
				for (page = 0; page < nand->info.pagesPerBlock; page++) {
					rpage = blockBasePage + page;
					err = nandfctrl2_pageRead(nand, rpage, nand->blockBuf + (page * nand->info.writesz));
					if (err == -ETIME) {
						(void)nandfctrl2_resetFlash(nand);
						return err;
					}
					if (err < 0) {
						break;
					}
				}

				if (err == 0) {
					err = nandfctrl2_eraseBlock(nand, block);
					if (err == -ETIME) {
						(void)nandfctrl2_resetFlash(nand);
						return err;
					}
				}

				if (err < 0) {
					/* Flash reported failure, mark block as bad and try next */
					if (nandfctrl2_markbad(nand, block) < 0) {
						return -EIO;
					}
					continue;
				}

				nand->blockBufNum = block;
			}

			size = min(len - ret, nand->info.erasesz - boffs);
			hal_memset(nand->blockBuf + boffs, NAND_ERASED_STATE, size);
			ret += size;

			nand->blockBufDirty = 1;

			boffs = 0;
		}
		else {
			/* Full block erase */
			if (nand->blockBufNum == block) {
				invalidateBlockBuf(nand);
			}

			err = nandfctrl2_eraseBlock(nand, block);
			if (err == -ETIME) {
				(void)nandfctrl2_resetFlash(nand);
				return err;
			}
			if (err < 0) {
				/* Flash reported erase failure, mark block as bad */
				if (nandfctrl2_markbad(nand, block) < 0) {
					return -EIO;
				}
			}
			else {
				ret += nand->info.erasesz;
			}
		}
	}

	return ret;
}


static int data_map(unsigned int minor, addr_t addr, size_t sz, int mode, addr_t memaddr, size_t memsz, int memmode, addr_t *a)
{
	(void)addr;
	(void)sz;
	(void)memaddr;
	(void)memsz;
	(void)a;

	if (minor >= NAND_DIE_CNT) {
		return -EINVAL;
	}

	/* Device mode cannot be higher than map mode to copy data */
	if ((mode & memmode) != mode) {
		return -EINVAL;
	}

	/* NAND is not mappable to any region */
	return dev_isNotMappable;
}


static int data_done(unsigned int minor)
{
	return data_sync(minor);
}


static int data_init(unsigned int minor)
{
	int err;
	nand_die_t *nand = nand_get(minor);

	if (nand == NULL) {
		return -ENODEV;
	}

	nand->minor = minor;
	nand->blockBufDirty = 0;
	nand->blockBufNum = (u32)-1;
	hal_memset(nand->blockBuf, NAND_ERASED_STATE, sizeof(nand->blockBuf));

	err = nandfctrl2_init(nand);
	if (err < 0) {
		lib_printf("\ndev/flash/nand: (%d.%d) Initialization error: %d", DEV_NAND_DATA, minor, err);
	}
	else {
		lib_printf("\ndev/flash/nand: Configured %s (%d.%d)", nand->info.name, DEV_NAND_DATA, minor);
	}

	return err;
}


__attribute__((constructor)) static void data_register(void)
{
	static const dev_ops_t opsDataNANDFCTRL2 = {
		.read = data_read,
		.write = data_write,
		.sync = data_sync,
		.map = data_map,
		.erase = data_erase,
	};

	static const dev_t devDataNANDFCTRL2 = {
		.name = "flash-nandfctrl2-data",
		.init = data_init,
		.done = data_done,
		.ops = &opsDataNANDFCTRL2,
	};

	devs_register(DEV_NAND_DATA, NAND_DIE_CNT, &devDataNANDFCTRL2);
}
