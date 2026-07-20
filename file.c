// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>

#include "ouichefs.h"
#include "bitmap.h"
#include "extent_ioctl.h"


/*
 * Map the buffer_head passed in argument with the iblock-th block of the file
 * represented by inode. If the requested block is not allocated and create is
 * true, allocate a new block on disk and map it.
 */
static int ouichefs_file_get_block(struct inode *inode, sector_t iblock,
				   struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct ouichefs_file_index_block *index;
	struct buffer_head *bh_index;
	int ret = 0, bno;

	/* If block number exceeds filesize, fail */
	if (iblock >= OUICHEFS_MAX_EXTENTS)
		return -EFBIG;

	/* Read index block from disk */
	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index)
		return -EIO;
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	/*
	 * Check if iblock is already allocated. If not and create is true,
	 * allocate it. Else, get the physical block number.
	 */
	if (index->extents[iblock].start == 0) {
		if (!create) {
			ret = 0;
			goto brelse_index;
		}

		bno = get_free_block(sbi);
		if (!bno) {
			ret = -ENOSPC;
			goto brelse_index;
		}

		index->extents[iblock].start = cpu_to_le32(bno);
		index->extents[iblock].count = cpu_to_le32(1);
		++inode->i_blocks;

		mark_inode_dirty(inode);
		mark_buffer_dirty(bh_index);
	} else {
		bno = le32_to_cpu(index->extents[iblock].start);
	}

	/* Map the physical block to the given buffer_head */
	map_bh(bh_result, sb, bno);

brelse_index:
	brelse(bh_index);

	return ret;
}

/*
 * Called by the page cache to read a page from the physical disk and map it in
 * memory.
 */
static void ouichefs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, ouichefs_file_get_block);
}

/*
 * Called by the page cache to write a dirty page to the physical disk (when
 * sync is called or when memory is needed).
 */
static int ouichefs_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, ouichefs_file_get_block, wbc);
}

/*
 * Called by the VFS when a write() syscall occurs on file before writing the
 * data in the page cache. This functions checks if the write will be able to
 * complete and allocates the necessary blocks through block_write_begin().
 */
static int ouichefs_write_begin(struct file *file,
				struct address_space *mapping, loff_t pos,
				unsigned int len, struct page **pagep,
				void **fsdata)
{
	int err;

	/* prepare the write */
	err = block_write_begin(mapping, pos, len, pagep,
				ouichefs_file_get_block);
	/* if this failed, reclaim newly allocated blocks */
	if (err < 0) {
		truncate_pagecache(file->f_inode, file->f_inode->i_size);
		if (ouichefs_truncate(file->f_inode) < 0)
			pr_err("%s:%d: truncate failed\n", __func__, __LINE__);
		goto out;
	}

	return 0;

out:
	return err;
}

/*
 * Called by the VFS after writing data from a write() syscall to the page
 * cache. This functions updates inode metadata and truncates the file if
 * necessary.
 */
static int ouichefs_write_end(struct file *file, struct address_space *mapping,
			      loff_t pos, unsigned int len, unsigned int copied,
			      struct page *page, void *fsdata)
{
	int ret;
	struct inode *inode = file->f_inode;

	/* Complete the write() */
	ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
	if (ret < len) {
		pr_err("%s:%d: wrote less than asked... what do I do? nothing for now...\n",
		       __func__, __LINE__);
	} else {
		/* Update inode metadata */
		inode->i_mtime = inode_set_ctime_current(inode);
		mark_inode_dirty(inode);
	}

	return ret;
}

/* Translate extent list to physical blocks */
static uint32_t ouichefs_extent_get_block(struct ouichefs_extent *extents, uint32_t logical_block)
{
	if (!extents)
		return 0;
	for (uint32_t i = 0; i < OUICHEFS_MAX_EXTENTS; i++) {
		uint32_t start = le32_to_cpu(extents[i].start);
		uint32_t count = le32_to_cpu(extents[i].count);
		
		if (count == 0)
			return 0;
		if (logical_block < count)
			return start + logical_block;
		/* Look up the physical block in the next extend */
		logical_block -= count;
	}

	return 0;
}

ssize_t ouichefs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	struct inode *inode = file->f_inode;
	struct super_block *sb = inode->i_sb;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct buffer_head *bh_index = sb_bread(sb, ci->index_block);

	if (!bh_index)
		return -EIO;
	struct ouichefs_file_index_block *index = (struct ouichefs_file_index_block *)bh_index->b_data;

	char *cursor = buf;
	char *buf_end = buf + count;

	for (int to_read; cursor < buf_end && *pos < inode->i_size; *pos += to_read, cursor += to_read) {
		sector_t iblock = *pos / sb->s_blocksize;
		int block_offset = *pos % sb->s_blocksize;
		int block_space = sb->s_blocksize - block_offset;
		int file_space = inode->i_size - *pos;
		int buf_space = buf_end - cursor;

		to_read = min(buf_space, min(block_space, file_space));

		// Have to think about sending an error but I guess it is fine
		// Doesnt make any sense now with extents
		// if (iblock >= OUICHEFS_MAX_EXTENTS)
		// 	break;
		// sector_t pblock = le32_to_cpu(index->blocks[iblock].start);
		sector_t pblock = ouichefs_extent_get_block(index->extents, iblock);
		if (pblock == 0) {
			clear_user(cursor, to_read);
		} else {
			struct buffer_head *bh = sb_bread(sb, pblock);
			if (!bh) {
				brelse(bh_index);
				return -EIO;
			}
			if (copy_to_user(cursor, bh->b_data + block_offset, to_read)) {
				brelse(bh);
				brelse(bh_index);
				return -EFAULT;
			}
			brelse(bh);
		}
	}

	brelse(bh_index);
	return cursor - buf;
}

ssize_t ouichefs_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	struct inode *inode = file->f_inode;
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);

	// removes blocks after end of file; filesize was already correctly set before write is called
	ouichefs_truncate(inode);
	// Keep in mind this could cause a race condition if no locks before
	// inode_lock(inode);
	if (file->f_flags & O_APPEND)
		*pos = inode->i_size;
	// inode_unlock(inode);

	struct buffer_head *bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index)
		return -EIO;
	struct ouichefs_file_index_block *index = (struct ouichefs_file_index_block *)bh_index->b_data;

	const char *cursor = buf;
	const char *buf_end = buf + count;

	for (int to_write; cursor < buf_end; *pos += to_write, cursor += to_write) {
		sector_t iblock = *pos / sb->s_blocksize;
		int block_offset = *pos % sb->s_blocksize;
		int block_space = sb->s_blocksize - block_offset;
		int buf_space = buf_end - cursor;
		to_write = min(buf_space, block_space);

		sector_t pblock = ouichefs_extent_get_block(index->extents, iblock);
		struct buffer_head *bh;
		if (pblock == 0) {
			pblock = get_free_block(sbi);
			if (!pblock) {
				brelse(bh_index);
				return -ENOSPC;
			}

			// We assume sequential writes; so we can assume that we reached the end of the stored extents
			struct ouichefs_extent *last_extent = &index->extents[index->num_extents - 1];
			if (index->num_extents > 0 && last_extent->start + last_extent->count == pblock) {
				last_extent->count++;
			} else {
				if (index->num_extents >= OUICHEFS_MAX_EXTENTS) {
					brelse(bh_index);
					put_block(sbi, pblock);
					return -ENOSPC;
				}
				struct ouichefs_extent *extent = &index->extents[index->num_extents];
				extent->start = cpu_to_le32(pblock);
				extent->count = cpu_to_le32(1);
				index->num_extents++;
			}

			inode->i_blocks++;
			mark_inode_dirty(inode);
			mark_buffer_dirty(bh_index);
			sync_dirty_buffer(bh_index);

			bh = sb_getblk(sb, pblock);
			if (!bh) {
				brelse(bh_index);
				return -EIO;
			}
			memset(bh->b_data, 0, sb->s_blocksize); // New blocks have to be zeroed
			set_buffer_uptodate(bh);
		} else {
			bh = sb_bread(sb, pblock);
			if (!bh) {
				brelse(bh_index);
				return -EIO;
			}
		}

		if (copy_from_user(bh->b_data + block_offset, cursor, to_write)) {
			brelse(bh);
			brelse(bh_index);
			return -EFAULT;
		}

		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}

	inode->i_mtime = inode_set_ctime_current(inode);
	if (inode->i_size < *pos)
		inode->i_size = *pos;
	mark_inode_dirty(inode);
	brelse(bh_index);
	return cursor - buf;
}

long extents_ioctl(struct file *file_desc, unsigned int cmd, unsigned long usr_addr)
{
	if (cmd == OUICHEFS_IOC_GET_EXTENTS) {
		struct inode *inode = file_desc->f_inode;
		struct super_block *sb = inode->i_sb;
		struct ouichefs_inode_info *inode_info = OUICHEFS_INODE(inode);
		struct ouichefs_file_index_block *index;
		struct buffer_head *bh_index;

		bh_index = sb_bread(sb, inode_info->index_block);
		if (!bh_index)
			return -EIO;
		index = (struct ouichefs_file_index_block *)bh_index->b_data;

		printk("ouichefs: extents for inode %lu: %du extent(s)\n", inode->i_ino, index->num_extents);
		for (size_t i = 0; i < OUICHEFS_MAX_EXTENTS; ++i) {
			struct ouichefs_extent extent = index->extents[i];
			if (extent.count == 0)
				break;
			printk("  [%lu] start=%d count=%d (blocks %d-%d)\n", i,
			       extent.start, extent.count, extent.start,
			       extent.start + max(extent.count - 1, 0));
		}
		brelse(bh_index);
	} else {
		return -ENOTTY;
	}
	return 0;
}

const struct address_space_operations ouichefs_aops = {
	.readahead = ouichefs_readahead,
	.writepage = ouichefs_writepage,
	.write_begin = ouichefs_write_begin,
	.write_end = ouichefs_write_end
};

const struct file_operations ouichefs_file_ops = {
	.owner = THIS_MODULE,
	.read = ouichefs_read,
	.write = ouichefs_write,
	.llseek = generic_file_llseek,
	.read_iter = generic_file_read_iter,
	.write_iter = generic_file_write_iter,
	.fsync = generic_file_fsync,
	.unlocked_ioctl = extents_ioctl,
};

int ouichefs_truncate(struct inode *inode)
{
	int ret;
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *inode_info = OUICHEFS_INODE(inode);
	struct buffer_head *bh;
	size_t next_num_blocks;

	bh = sb_bread(sb, inode_info->index_block);
	if (!bh) {
		ret = -EIO;
		goto out;
	}

	ret = block_truncate_page(inode->i_mapping, inode->i_size, ouichefs_file_get_block);
	if (ret < 0)
		goto out_brelse;

	struct ouichefs_file_index_block *index = (struct ouichefs_file_index_block *)bh->b_data;

	next_num_blocks = (inode->i_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
	for (size_t i = next_num_blocks; i < OUICHEFS_MAX_EXTENTS; ++i) {
		uint32_t bno = le32_to_cpu(index->extents[i].start);

		if (!bno)
			continue;

		put_block(sbi, bno);
		--inode->i_blocks;

		index->extents[i].start = cpu_to_le32(0);
		index->extents[i].count = cpu_to_le32(0);
	}

	mark_buffer_dirty(bh);
	brelse(bh);

	mark_inode_dirty(inode);

	return 0;

out_brelse:
	brelse(bh);
out:
	return ret;
}
