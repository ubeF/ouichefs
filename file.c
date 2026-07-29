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

static uint32_t ouichefs_alloc(struct inode *inode, uint32_t requested, uint32_t *block)
{
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

	/* Zero out newly allocated blocks */
	for (uint32_t i = 0; i < allocated_count; i++) {
		struct buffer_head *bh = sb_getblk(sb, allocated_start + i);
		if (!bh)
			continue;

		lock_buffer(bh);
		memset(bh->b_data, 0, sb->s_blocksize);
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}

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
	if (!index->extents[iblock].start) {
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

void ouichefs_garbage_collector(struct super_block *sb)
{
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
	uint32_t num_extents = le32_to_cpu(index->num_extents);
	struct ouichefs_extent *list = index->extents;
	if (num_extents >= OUICHEFS_MAX_EXTENTS) {
		pr_err("file reached maximum number of extents\n");
		return -ENOSPC;
	}

	if (extent_idx < num_extents)
		memmove(&list[extent_idx + 1], &list[extent_idx],
			(num_extents - extent_idx) * sizeof(*list));

	index->num_extents = cpu_to_le32(num_extents + 1);
	return extent_idx;
}

ssize_t ouichefs_shift_extents_left(struct ouichefs_file_index_block *index, size_t extent_idx)
{
	uint32_t num_extents = le32_to_cpu(index->num_extents);
	struct ouichefs_extent *list = index->extents;
	if (extent_idx >= num_extents) {
		pr_err("invalid extent index for left shift\n");
		return -EINVAL;
	}

	if (extent_idx + 1 < num_extents) {
		memmove(&list[extent_idx], &list[extent_idx + 1],
			(num_extents - extent_idx - 1) * sizeof(*list));
	} else {
		list[extent_idx].start = 0;
		list[extent_idx].count = 0;
	}

	index->num_extents = cpu_to_le32(num_extents - 1);
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
	uint32_t old_count = le32_to_cpu(list[extent_idx].count);
	uint32_t consumed = block_idx + le32_to_cpu(new.count);
	uint32_t tail_count = old_count > consumed ? old_count - consumed : 0;

	if (block_idx == 0) {
		/*
			If the new extent is contiguous with the previous extent (and it is not a hole),
			merge them and keep only the trailing hole (if any).
		*/
		if (extent_idx > 0 && le32_to_cpu(list[extent_idx - 1].start) &&
	    le32_to_cpu(list[extent_idx - 1].start) + le32_to_cpu(list[extent_idx - 1].count) == le32_to_cpu(new.start)) {
			list[extent_idx - 1].count = cpu_to_le32(le32_to_cpu(list[extent_idx - 1].count) + le32_to_cpu(new.count));

			if (tail_count > 0) {
				/* Keep the remaining tail as a new hole extent. */
				list[extent_idx].start = 0;
				list[extent_idx].count = cpu_to_le32(tail_count);
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
			list[extent_idx + 1].count = cpu_to_le32(tail_count);
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
	list[extent_idx].count = cpu_to_le32(block_idx);

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
		list[extent_idx + 2].count = cpu_to_le32(tail_count);
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
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
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
	ssize_t old_num_extents = le32_to_cpu(index->num_extents);

	uint32_t start_block = *pos / sb->s_blocksize;
	uint64_t end_pos = *pos + count - 1;
	uint32_t end_block = end_pos / sb->s_blocksize;

	/* Creating a filling hole when we write past the end of file */
	if (end_block >= inode->i_blocks - 1) {
		uint32_t num_extents = le32_to_cpu(index->num_extents);
		if (num_extents > 0 && !extents[num_extents - 1].start) {
			/* If the last extent is a hole, we can just extend it */
			uint32_t new_count = le32_to_cpu(extents[num_extents - 1].count) +
				end_block - (inode->i_blocks - 2);
			extents[num_extents - 1].count = cpu_to_le32(new_count);
		} else {
			/*
				This could cause us to lose an extent under extreme fragmentation.
				If the new blocks would be contiguous with the last extent, we wrongfully fail here.
				But this is unlikely under extreme fragmentation. So we will ignore this for now.
			*/
			if (num_extents >= OUICHEFS_MAX_EXTENTS) {
				pr_err("file reached maximum number of extents\n");
				brelse(bh_index);
				inode_unlock(inode);
				return -ENOSPC;
			}
			struct ouichefs_extent *new_extent = &extents[num_extents++];
			new_extent->start = 0;
			new_extent->count = cpu_to_le32(end_block - (inode->i_blocks - 2));
			index->num_extents = cpu_to_le32(num_extents);
		}
		inode->i_blocks = end_block + 2; // Holes are also counted in i_blocks, dont forget the index block
		mark_inode_dirty(inode);
	}

	size_t extent_idx = 0;
	size_t block_idx = 0;
	while (extent_idx < le32_to_cpu(index->num_extents) &&
	       block_idx + le32_to_cpu(extents[extent_idx].count) < start_block) {
		block_idx += le32_to_cpu(extents[extent_idx].count);
		extent_idx++;
	}
	uint32_t block_offset = start_block - block_idx;

	/* Allocate blocks */
	for (size_t blocks_to_write = 0; start_block <= end_block; start_block += blocks_to_write) {
		struct ouichefs_extent *extent = &extents[extent_idx];
		uint32_t extent_count = le32_to_cpu(extent->count);
		uint32_t extent_start = le32_to_cpu(extent->start);
		blocks_to_write = min(extent_count - block_offset, end_block - start_block + 1);
		/* If there is no hole, skip */
		if (extent_start) {
			extent_idx++;
		} else {
			/* If there is a hole, allocate blocks to fill it */
			struct ouichefs_extent new_extent;
			new_extent.count = ouichefs_alloc(inode, blocks_to_write, &new_extent.start);
			if (!new_extent.count)
				break;
			extent_idx = ouichefs_insert_extent(index, new_extent, extent_idx, block_offset);
			if (extent_idx < 0)
				break;
		}
		block_offset = 0; // Reset block_offset after the first iteration
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
			pr_err("ouichefs: we hit a hole when writing block=%u\n", iblock);
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

	sbi->nr_extents += le32_to_cpu(index->num_extents) - old_num_extents;

	inode_unlock(inode);
	if (get_fragmentation(sb) >= fragmentation_threshold)
		ouichefs_fs_defrag(sb);
	return result;
}

ssize_t ouichefs_copy_block(struct super_block *sb, uint32_t source_block,
			    uint32_t destination_block)
{
	struct buffer_head *source_bh;
	struct buffer_head *destination_bh;
	int ret = 0;

	source_bh = sb_bread(sb, source_block);
	if (!source_bh)
		return -EIO;

	destination_bh = sb_getblk(sb, destination_block);
	if (!destination_bh) {
		brelse(source_bh);
		return -ENOMEM;
	}

	lock_buffer(destination_bh);

	memcpy(destination_bh->b_data, source_bh->b_data, sb->s_blocksize);

	set_buffer_uptodate(destination_bh);
	mark_buffer_dirty(destination_bh);
	unlock_buffer(destination_bh);
	sync_dirty_buffer(destination_bh);

	brelse(destination_bh);
	brelse(source_bh);
	return ret;
}

ssize_t file_defrag(struct super_block *sb, struct inode *inode)
{
	struct ouichefs_inode_info *inode_info = OUICHEFS_INODE(inode);
	struct ouichefs_file_index_block *index;
	struct buffer_head *bh_index;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	bh_index = sb_bread(sb, inode_info->index_block);
	if (!bh_index) {
		printk("ouichefs: unable to acces index block\n");
		return -EIO;
	}
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	/* Count total blocks and extra extents needed because of holes */
	uint32_t needed_blocks = 0;

	for (uint32_t i = 0; i < le32_to_cpu(index->num_extents); i++) {
		uint32_t start = le32_to_cpu(index->extents[i].start);
		uint32_t count = le32_to_cpu(index->extents[i].count);
		if (!start) {
			continue;
		}
		needed_blocks += count;
	}

	if (needed_blocks == 0) {
		brelse(bh_index);
		return 0;
	}

	/* Try to allocate contiguous amount of blocks */
	uint32_t new_block_start;
	uint32_t allocated_blocks = ouichefs_alloc_contiguous(sb, needed_blocks, &new_block_start);
	if (!allocated_blocks) {
		brelse(bh_index);
		return -ENOSPC;
	}
	/* Create a new extent list */
	struct ouichefs_extent *new_extent_list = kzalloc(sizeof(index->extents), GFP_KERNEL);
	uint32_t new_extent_index = 0;
	uint32_t old_extent_index = 0;

	/* Copy as much data as possible */
	for (uint32_t blocks_copied = 0; old_extent_index < le32_to_cpu(index->num_extents) && blocks_copied < allocated_blocks; old_extent_index++) {
		uint32_t old_start = le32_to_cpu(index->extents[old_extent_index].start);
		uint32_t old_count = le32_to_cpu(index->extents[old_extent_index].count);

		/* Hole we add extent with same count */
		if (!old_start) {
			if (le32_to_cpu(new_extent_list[new_extent_index].start))
				++new_extent_index;
			new_extent_list[new_extent_index].count = cpu_to_le32(le32_to_cpu(new_extent_list[new_extent_index].count) + old_count);
		} else {
			if (!le32_to_cpu(new_extent_list[new_extent_index].start)) {
				if (le32_to_cpu(new_extent_list[new_extent_index].count))
					++new_extent_index;
				new_extent_list[new_extent_index].start = cpu_to_le32(new_block_start + blocks_copied);
			}
			for (uint32_t j = 0; j < old_count; j++) {
				if (blocks_copied + 1 > allocated_blocks) {
					/* We have run out of allocated blocks, we need to create a new extent for the remaining blocks */
					new_extent_index++;
					new_extent_list[new_extent_index].start = cpu_to_le32(old_start + j);
					new_extent_list[new_extent_index].count = cpu_to_le32(old_count - j);
					break;
				}

				if (ouichefs_copy_block(sb, old_start + j, new_block_start + blocks_copied))
					goto restore_old;
				blocks_copied++;
				new_extent_list[new_extent_index].count = cpu_to_le32(le32_to_cpu(new_extent_list[new_extent_index].count) + 1);
			}
		}
	}
	new_extent_index++;

	while (old_extent_index < le32_to_cpu(index->num_extents)) {
		new_extent_list[new_extent_index++] = index->extents[old_extent_index++];
	}

	/*
	 * If copying succeded:
	 * Free all the copied blocks and set the new extent list.
	 */
	uint32_t freed_blocks = 0;
	for (uint32_t i = 0; i < le32_to_cpu(index->num_extents) && freed_blocks < allocated_blocks; i++) {
		uint32_t old_start = le32_to_cpu(index->extents[i].start);
		uint32_t old_count = le32_to_cpu(index->extents[i].count);

		for (uint32_t j = 0; j < old_count && freed_blocks < allocated_blocks; j++) {
			if (!old_start)
				continue;
			put_block(sbi, old_start + j);
			freed_blocks++;
		}
	}

	sbi->nr_extents += new_extent_index - le32_to_cpu(index->num_extents);
	index->num_extents = cpu_to_le32(new_extent_index);
	memcpy(&index->extents, new_extent_list, sizeof(index->extents));

	mark_buffer_dirty(bh_index);
	sync_dirty_buffer(bh_index);
	kfree(new_extent_list);
	brelse(bh_index);
	return allocated_blocks;

restore_old:
	for (uint32_t i = 0; i < allocated_blocks; i++) {
		put_block(sbi, new_block_start + i);
	}
	brelse(bh_index);
	kfree(new_extent_list);
	return 0;
}

void ouichefs_fs_defrag(struct super_block *sb)
{
	struct inode *inode;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	for (uint32_t ino = 1; ino < sbi->nr_inodes; ino++) {
		if (test_bit(ino, sbi->ifree_bitmap))
			continue;

		inode = ouichefs_iget(sb, ino);
		if (IS_ERR(inode))
			continue;
		if (S_ISREG(inode->i_mode)) {
			file_defrag(sb, inode);
		}
		iput(inode);
	}
}

long extents_ioctl(struct file *file_desc, unsigned int cmd, unsigned long usr_addr)
{
	struct inode *inode = file_desc->f_inode;
	struct super_block *sb = inode->i_sb;

	if (cmd == OUICHEFS_IOC_GET_EXTENTS) {
		struct ouichefs_inode_info *inode_info = OUICHEFS_INODE(inode);
		struct ouichefs_file_index_block *index;
		struct buffer_head *bh_index;

		bh_index = sb_bread(sb, inode_info->index_block);
		if (!bh_index) {
			printk("ouichefs: unable to acces index block\n");
			return -EIO;
		}
		index = (struct ouichefs_file_index_block *)bh_index->b_data;

		printk("ouichefs: extents for inode %lu: %d extent(s)\n", inode->i_ino, le32_to_cpu(index->num_extents));
		for (size_t i = 0; i < OUICHEFS_MAX_EXTENTS; ++i) {
			struct ouichefs_extent extent = index->extents[i];
			uint32_t extent_start = le32_to_cpu(extent.start);
			uint32_t extent_count = le32_to_cpu(extent.count);
			if (extent_count == 0)
				break;
			printk("  [%lu] start=%d count=%d (blocks %d-%d)\n", i,
			       extent_start, extent_count, extent_start,
			       extent_start + max(extent_count - 1, 0));
		}
		brelse(bh_index);
	} else if (cmd == OUICHEFS_IOC_DEFRAG_FILE) {
		return file_defrag(sb, inode);
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

	bh = sb_bread(sb, inode_info->index_block);
	if (!bh) {
		ret = -EIO;
		goto out;
	}

	ret = block_truncate_page(inode->i_mapping, inode->i_size, ouichefs_file_get_block);
	if (ret < 0)
		goto out_brelse;

	struct ouichefs_file_index_block *index = (struct ouichefs_file_index_block *)bh->b_data;

	uint32_t required_blocks = (inode->i_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;

	uint32_t remaining_blocks = required_blocks;
	uint32_t old_num_extents = le32_to_cpu(index->num_extents);
	uint32_t new_num_extents = 0;

	/*
	 * Go through all extents and only keep the needed extents.
	 * If keep is 0 remove extent.
	 * If at the end remaining_blocks is > 0 throw error since truncate wanted to increase file size.
	 */
	for (uint32_t i = 0; i < old_num_extents; i++) {
		struct ouichefs_extent *extent = &index->extents[i];
		uint32_t keep;

		uint32_t extent_count = le32_to_cpu(extent->count);
		uint32_t extent_start = le32_to_cpu(extent->start);

		/* Kept blocks of extent are either all of them or number of rest of needed blocks */
		keep = min(remaining_blocks, extent_count);

		if (extent_start) {
			for (uint32_t j = keep; j < extent_count; j++) {
				put_block(sbi, extent_start + j);
			}
		}

		/* Update remaining_blocks or free extent if keep is zero */
		if (keep > 0) {
			extent->count = cpu_to_le32(keep);
			new_num_extents = i + 1;
			remaining_blocks -= keep;
		} else {
			extent->start = 0;
			extent->count = 0;
		}
	}

	sbi->nr_extents += new_num_extents - old_num_extents;
	index->num_extents = cpu_to_le32(new_num_extents);

	mark_buffer_dirty(bh);
	brelse(bh);
	inode->i_blocks = required_blocks - remaining_blocks + 1; // +1 for index block
	mark_inode_dirty(inode);

	return 0;

out_brelse:
	brelse(bh);
out:
	return ret;
}
