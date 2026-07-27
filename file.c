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

static uint32_t ouichefs_alloc_contiguous(struct super_block *sb, uint32_t requested, uint32_t *block)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	unsigned long *freemap = sbi->bfree_bitmap;
	uint32_t nr_blocks = sbi->nr_blocks;
	uint32_t longest_run_start = 0;
	uint32_t longest_run_count = 0;
	// Find longest run; return early when request-sized run is found
	for (uint32_t end, start = 0; start < nr_blocks; start = end) {
		start = find_next_bit(freemap, nr_blocks, start);
		if (start == nr_blocks)
			break;
		end = find_next_zero_bit(freemap, nr_blocks, start);

		uint32_t diff = min(requested, end - start);

		if (diff > longest_run_count) {
			if (longest_run_count > 0) {
				bitmap_set(freemap, longest_run_start, longest_run_count); // remove previous "reservation"
				sbi->nr_free_blocks += longest_run_count;
			}
			longest_run_start = start;
			longest_run_count = diff;
			bitmap_clear(freemap, longest_run_start, longest_run_count);
			sbi->nr_free_blocks -= longest_run_count;
		}

		if (diff == requested)
			break;
	}

	if (longest_run_count > 0) {
		*block = longest_run_start;
		return longest_run_count;
	}

	pr_err("unable to allocate blocks\n");
	return 0;
}

static uint32_t ouichefs_alloc(struct inode *inode, uint32_t requested, uint32_t *block) {
	struct super_block *sb = inode->i_sb;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);

	uint32_t allocated_start, allocated_count;

	if (ci->i_reserved_count == 0) {
		allocated_count = ouichefs_alloc_contiguous(sb, max(requested, reservation_size), &allocated_start);
		if (!allocated_count) {
			ouichefs_garbage_collector(sb);
			allocated_count = ouichefs_alloc_contiguous(sb, max(requested, reservation_size), &allocated_start);
			if (!allocated_count)
				return 0;
		}
		ci->i_reserved_count = allocated_count;
		ci->i_reserved_start = allocated_start;
	}

	allocated_start = ci->i_reserved_start;
	allocated_count = min(ci->i_reserved_count, requested);

	ci->i_reserved_start += allocated_count;
	ci->i_reserved_count -= allocated_count;

	*block = allocated_start;

	mark_inode_dirty(inode);
	return allocated_count;
}

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

		index->extents[iblock].start = bno;
		index->extents[iblock].count = 1;
		++inode->i_blocks;

		mark_inode_dirty(inode);
		mark_buffer_dirty(bh_index);
	} else {
		bno = index->extents[iblock].start;
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
		uint32_t start = extents[i].start;
		uint32_t count = extents[i].count;

		if (count == 0)
			return 0;
		if (logical_block < count) {
			if (start == 0)
				return 0;
			return start + logical_block;
		}
		/* Look up the physical block in the next extend */
		logical_block -= count;
	}

	return 0;
}

int ouichefs_release(struct inode *inode, struct file *file)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);

	for (uint32_t i = 0; i < ci->i_reserved_count; i++) {
		put_block(sbi, ci->i_reserved_start + i);
	}

	ci->i_reserved_count = 0;
	ci->i_reserved_start = 0;

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

void ouichefs_garbage_collector(struct super_block *sb) {
	struct inode *cur;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	sbi->nr_gc_runs++;
	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(cur, &sb->s_inodes, i_sb_list) {
		struct ouichefs_inode_info *ci = OUICHEFS_INODE(cur);
		if (ci->i_reserved_count > 0) {
			inode_lock(cur);
			for (uint32_t i = 0; i < ci->i_reserved_count; i++) {
				put_block(sbi, ci->i_reserved_start + i);
			}

			ci->i_reserved_count = 0;
			ci->i_reserved_start = 0;
			inode_unlock(cur);
		}
	}
	spin_unlock(&sb->s_inode_list_lock);
}

ssize_t ouichefs_shift_extents_right(struct ouichefs_file_index_block *index, size_t extent_idx)
{
	struct ouichefs_extent *list = index->extents;
	if (index->num_extents >= OUICHEFS_MAX_EXTENTS) {
		pr_err("file reached maximum number of extents\n");
		return -ENOSPC;
	}

	if (extent_idx < index->num_extents)
		memmove(&list[extent_idx + 1], &list[extent_idx],
			(index->num_extents - extent_idx) * sizeof(*list));

	index->num_extents++;
	return extent_idx;
}

ssize_t ouichefs_shift_extents_left(struct ouichefs_file_index_block *index, size_t extent_idx)
{
	struct ouichefs_extent *list = index->extents;
	if (extent_idx >= index->num_extents) {
		pr_err("invalid extent index for left shift\n");
		return -EINVAL;
	}

	if (extent_idx + 1 < index->num_extents)
		memmove(&list[extent_idx], &list[extent_idx + 1],
			(index->num_extents - extent_idx - 1) * sizeof(*list));

	index->num_extents--;
	return extent_idx;
}

ssize_t ouichefs_insert_extent(struct ouichefs_file_index_block *index, struct ouichefs_extent new, size_t extent_idx, size_t block_idx)
{
	struct ouichefs_extent *list = index->extents;
	/*
	 * The current extent is treated as a hole. We split it into:
	 *   [prefix hole] [new extent] [trailing hole]
	 * depending on where the new extent starts and how many blocks it uses.
	 */
	uint32_t old_count = list[extent_idx].count;
	uint32_t consumed = block_idx + new.count;
	uint32_t tail_count = old_count > consumed ? old_count - consumed : 0;

	if (block_idx == 0) {
		/*
		 * If the new extent is contiguous with the previous extent,
		 * merge them and keep only the trailing hole (if any).
		 */
		if (extent_idx > 0 && list[extent_idx - 1].start + list[extent_idx - 1].count == new.start) {
			list[extent_idx - 1].count += new.count;

			if (tail_count > 0) {
				/* Keep the remaining tail as a new hole extent. */
				list[extent_idx].start = 0;
				list[extent_idx].count = tail_count;
				return extent_idx + 1;
			} else {
				/* No tail hole remains: remove the old hole extent. */
				ouichefs_shift_extents_left(index, extent_idx);
				return extent_idx;
			}
		}

		/* Insert the new extent in place of the current hole. */
		list[extent_idx] = new;
		if (tail_count > 0) {
			/* Preserve the remaining tail as a new hole extent. */
			int ret = ouichefs_shift_extents_right(index, extent_idx + 1);
			if (ret < 0)
				return ret;
			list[extent_idx + 1].start = 0;
			list[extent_idx + 1].count = tail_count;
			return extent_idx + 2;
		} else {
			/* No tail hole remains: done. */
			return extent_idx + 1;
		}
	}

	/*
	 * The new extent starts inside the existing hole, so keep the prefix
	 * of the hole at the current slot and insert the new extent after it.
	 */
	list[extent_idx].count = block_idx;

	int ret = ouichefs_shift_extents_right(index, extent_idx + 1);
	if (ret < 0)
		return ret;

	list[extent_idx + 1] = new;

	if (tail_count > 0) {
		/* Leave a trailing hole extent after the newly inserted extent. */
		int ret = ouichefs_shift_extents_right(index, extent_idx + 2);
		if (ret < 0)
			return ret;
		list[extent_idx + 2].start = 0;
		list[extent_idx + 2].count = tail_count;
		return extent_idx + 3;
	} else {
		/* No tail hole remains: done. */
		return extent_idx + 2;
	}
}

ssize_t ouichefs_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	struct inode *inode = file->f_inode;
	struct super_block *sb = inode->i_sb;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);

	inode_lock(inode);
	if (file->f_flags & O_APPEND)
		*pos = inode->i_size;

	struct buffer_head *bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index) {
		pr_err("failed to read index block\n");
		inode_unlock(inode);
		return -EIO;
	}

	struct ouichefs_file_index_block *index = (struct ouichefs_file_index_block *)bh_index->b_data;
	struct ouichefs_extent *extents = index->extents;

	uint32_t start_block = *pos / sb->s_blocksize;
	uint64_t end_pos = *pos + count;
	uint32_t end_block = end_pos / sb->s_blocksize;

	/* Creating a filling hole when we write past the end of file */
	if (end_block >= inode->i_blocks) {
		if (index->num_extents > 0 && extents[index->num_extents - 1].start == 0) {
			/* If the last extent is a hole, we can just extend it */
			extents[index->num_extents - 1].count = end_block - (inode->i_blocks - 1);
		} else {
			/* 
				This could cause us to lose an extent under extreme fragmentation.
				If the new blocks would be contiguous with the last extent, we wrongfully fail here. 
				But this is unlikely under extreme fragmentation. So we will ignore this for now.
			*/
			if (index->num_extents >= OUICHEFS_MAX_EXTENTS) {
				pr_err("file reached maximum number of extents\n");
				brelse(bh_index);
				inode_unlock(inode);
				return -ENOSPC;
			}
			struct ouichefs_extent *new_extent = &extents[index->num_extents++];
			new_extent->start = 0;
			new_extent->count = end_block - (inode->i_blocks - 1);
		}
		inode->i_blocks = end_block + 1; // Holes are also counted in i_blocks
		mark_inode_dirty(inode);
	}

	size_t extent_idx = 0;
	size_t block_idx = 0;
	while (block_idx + extents[extent_idx].count < start_block) {
		block_idx += extents[extent_idx].count;
		extent_idx++;
	}
	uint32_t block_offset = start_block - block_idx;

	/* Allocate blocks */
	for (size_t extent_space = 0; start_block <= end_block; start_block += extent_space) {
		struct ouichefs_extent *extent = &extents[extent_idx];
		extent_space = extent->count - block_offset;
		block_offset = 0; // Reset block_offset after the first iteration
		/* If there is no hole, skip */
		if (extent->start) {
			extent_idx++;
		} else {
			/* If there is a hole, allocate blocks to fill it */
			size_t blocks_to_write = min(extent_space, end_block - start_block + 1);
			struct ouichefs_extent new_extent;
			new_extent.count = ouichefs_alloc(inode, blocks_to_write, &new_extent.start);
			if (!new_extent.count)
				break;
			extent_idx = ouichefs_insert_extent(index, new_extent, extent_idx, block_offset);
			if (extent_idx < 0)
				break;
		}
	}

	mark_buffer_dirty(bh_index);
	sync_dirty_buffer(bh_index);

	/* Write block */
	const char __user *cursor = buf;
	const char __user *buf_end = buf + count;

	int result = 0;
	for (int to_write; cursor < buf_end; *pos += to_write, cursor += to_write) {
		uint32_t iblock = *pos / sb->s_blocksize;
		int block_offset = *pos % sb->s_blocksize;
		int block_space = sb->s_blocksize - block_offset;
		int buf_space = buf_end - cursor;
		to_write = min(buf_space, block_space);

		uint32_t pblock = ouichefs_extent_get_block(extents, iblock);

		if (!pblock) {
			/* We hit a hole, all blocks are allocated */
			result = -ENOSPC;
			break;
		}

		struct buffer_head *bh = sb_bread(sb, pblock);
		if (!bh) {
			pr_err("ouichefs: failed to read block=%u\n", pblock);
			result = -EIO;
			break;
		}

		if (copy_from_user(bh->b_data + block_offset, cursor, to_write)) {
			brelse(bh);
			result = -EFAULT;
			break;
		}

		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}

	brelse(bh_index);

	if (cursor > buf) { // We wrote at least something 
		inode->i_mtime = inode_set_ctime_current(inode);
		inode->i_size = max(*pos, inode->i_size);
		mark_inode_dirty(inode);
		result = cursor - buf;
	} 

	inode_unlock(inode);
	return result;
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
		if (!bh_index) {
			printk("ouichefs: unable to acces index block\n");
			return -EIO;
		}
		index = (struct ouichefs_file_index_block *)bh_index->b_data;

		printk("ouichefs: extents for inode %lu: %d extent(s)\n", inode->i_ino, index->num_extents);
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
		printk("ouichefs: unknown ioctl\n");
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
	.release = ouichefs_release,
};

int ouichefs_truncate(struct inode *inode)
{
	int ret;
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *inode_info = OUICHEFS_INODE(inode);
	struct buffer_head *bh;
	uint32_t required_num_blocks;

	bh = sb_bread(sb, inode_info->index_block);
	if (!bh) {
		ret = -EIO;
		goto out;
	}

	ret = block_truncate_page(inode->i_mapping, inode->i_size, ouichefs_file_get_block);
	if (ret < 0)
		goto out_brelse;

	struct ouichefs_file_index_block *index = (struct ouichefs_file_index_block *)bh->b_data;

	required_num_blocks = (inode->i_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;

	uint32_t remaining_blocks = required_num_blocks;
	uint32_t old_num_extents = index->num_extents;
	uint32_t new_num_extents = 0;

	/*
	 * Go through all extents and only keep the needed extents.
	 * If keep is 0 remove extent.
	 * If at the end remaining_blocks is > 0 throw error since truncate wanted to increase file size.
	 */
	for (uint32_t i = 0; i < old_num_extents; i++) {
		struct ouichefs_extent *extent = &index->extents[i];
		uint32_t keep;

		/* Kept blocks of extent are either all of them or number of rest of needed blocks */
		keep = min(remaining_blocks, extent->count);

		if (extent->start) {
			for (uint32_t j = keep; j < extent->count; j++) {
				put_block(sbi, extent->start + j);
				inode->i_blocks--;
			}
		}

		/* Update remaining_blocks or free extent if keep is zero */
		if (keep > 0) {
			extent->count = keep;
			new_num_extents = i + 1;
			remaining_blocks -= keep;
		} else {
			extent->start = 0;
			extent->count = 0;
		}
	}

	if (remaining_blocks > 0) {
		pr_err("ouichefs: truncate attempted to grow file\n");
		ret = -EINVAL;
		goto out_brelse;
	}

	index->num_extents = new_num_extents;

	mark_buffer_dirty(bh);
	brelse(bh);
	mark_inode_dirty(inode);

	return 0;

out_brelse:
	brelse(bh);
out:
	return ret;
}
