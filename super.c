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
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/blkdev.h>

#include "ouichefs.h"
#include "bitmap.h"

static struct kmem_cache *ouichefs_inode_cache;

int ouichefs_init_inode_cache(void)
{
	ouichefs_inode_cache = kmem_cache_create(
		"ouichefs_cache", sizeof(struct ouichefs_inode_info), 0, 0,
		NULL);
	if (!ouichefs_inode_cache)
		return -ENOMEM;
	return 0;
}

void ouichefs_destroy_inode_cache(void)
{
	kmem_cache_destroy(ouichefs_inode_cache);
}

static struct inode *ouichefs_alloc_inode(struct super_block *sb)
{
	struct ouichefs_inode_info *ci;

	/* ci = kzalloc(sizeof(struct ouichefs_inode_info), GFP_KERNEL); */
	ci = kmem_cache_alloc(ouichefs_inode_cache, GFP_KERNEL);
	if (!ci)
		return NULL;
	inode_init_once(&ci->vfs_inode);
	return &ci->vfs_inode;
}

static void ouichefs_destroy_inode(struct inode *inode)
{
	struct ouichefs_inode_info *ci;

	ci = OUICHEFS_INODE(inode);
	kmem_cache_free(ouichefs_inode_cache, ci);
}

static int ouichefs_write_inode(struct inode *inode,
				struct writeback_control *wbc)
{
	struct ouichefs_inode *disk_inode;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh;
	uint32_t ino = inode->i_ino;
	uint32_t inode_block = (ino / OUICHEFS_INODES_PER_BLOCK) + 1;
	uint32_t inode_shift = ino % OUICHEFS_INODES_PER_BLOCK;

	if (ino >= sbi->nr_inodes)
		return 0;

	bh = sb_bread(sb, inode_block);
	if (!bh)
		return -EIO;
	disk_inode = (struct ouichefs_inode *)bh->b_data;
	disk_inode += inode_shift;

	/* update the mode using what the generic inode has */
	disk_inode->i_mode = cpu_to_le32(inode->i_mode);
	disk_inode->i_uid = cpu_to_le32(i_uid_read(inode));
	disk_inode->i_gid = cpu_to_le32(i_gid_read(inode));
	disk_inode->i_size = cpu_to_le32(inode->i_size);
	disk_inode->i_ctime = cpu_to_le32(inode_get_ctime_sec(inode));
	disk_inode->i_nctime = cpu_to_le64(inode_get_ctime_nsec(inode));
	disk_inode->i_atime = cpu_to_le32(inode->i_atime.tv_sec);
	disk_inode->i_natime = cpu_to_le64(inode->i_atime.tv_nsec);
	disk_inode->i_mtime = cpu_to_le32(inode->i_mtime.tv_sec);
	disk_inode->i_nmtime = cpu_to_le64(inode->i_mtime.tv_nsec);
	disk_inode->i_blocks = cpu_to_le32(inode->i_blocks);
	disk_inode->i_nlink = cpu_to_le32(inode->i_nlink);
	disk_inode->index_block = cpu_to_le32(ci->index_block);

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

static void ouichefs_evict_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *inode_info = OUICHEFS_INODE(inode);
	struct buffer_head *bh;
	struct ouichefs_file_index_block *file_index;
	uint32_t ino = inode->i_ino;
	uint32_t i, j;

	truncate_inode_pages_final(&inode->i_data);

	for (uint32_t i = 0; i < inode_info->i_reserved_count; i++) {
		put_block(sbi, inode_info->i_reserved_start + i);
	}

	inode_info->i_reserved_count = 0;
	inode_info->i_reserved_start = 0;

	/*
	 * Cleanup pointed blocks if file/directory is not linked anymore.
	 * If we fail to read the index block, cleanup inode anyway and
	 * lose this file/directory's blocks forever.
	 */
	if (!inode->i_nlink && inode_info->index_block) {
		bh = sb_bread(sb, inode_info->index_block);
		if (!bh) {
			pr_warn("failed to release index block\n");
			goto invalidate;
		}

		if (S_ISREG(inode->i_mode)) {
			file_index = (struct ouichefs_file_index_block *)bh->b_data;

			for (i = 0; i < OUICHEFS_MAX_EXTENTS; ++i) {
				if (!le32_to_cpu(file_index->extents[i].start))
					continue;
				for (j = 0; j < le32_to_cpu(file_index->extents[i].count); j++) {
					put_block(sbi, le32_to_cpu(file_index->extents[i].start) + j);
				}
			}
		}

		/*
		 * Make sure the buffer is not marked dirty anymore (and no writeback
		 * is in progress), as we don't want it to be written back when it might
		 * already be in use for something else (especially for the contents of a file)!
		 */
		lock_buffer(bh);
		clear_buffer_dirty(bh);
		unlock_buffer(bh);
		brelse(bh);

		put_block(sbi, inode_info->index_block);
		inode_info->index_block = 0;
	}

invalidate:
	invalidate_inode_buffers(inode);
	clear_inode(inode);

	if (!inode->i_nlink) {
		/* Free inode from bitmap */
		put_inode(sbi, ino);
	}
}

static int sync_sb_info(struct super_block *sb, int wait)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_sb_info *disk_sb;
	struct buffer_head *bh;

	/* Flush superblock */
	bh = sb_bread(sb, 0);
	if (!bh)
		return -EIO;
	disk_sb = (struct ouichefs_sb_info *)bh->b_data;

	disk_sb->nr_blocks = cpu_to_le32(sbi->nr_blocks);
	disk_sb->nr_inodes = cpu_to_le32(sbi->nr_inodes);
	disk_sb->nr_istore_blocks = cpu_to_le32(sbi->nr_istore_blocks);
	disk_sb->nr_ifree_blocks = cpu_to_le32(sbi->nr_ifree_blocks);
	disk_sb->nr_bfree_blocks = cpu_to_le32(sbi->nr_bfree_blocks);
	disk_sb->nr_free_inodes = cpu_to_le32(sbi->nr_free_inodes);
	disk_sb->nr_free_blocks = cpu_to_le32(sbi->nr_free_blocks);

	mark_buffer_dirty(bh);
	if (wait)
		sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

static int sync_ifree(struct super_block *sb, int wait)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh;
	int i, idx;

	/* Flush free inodes bitmask */
	for (i = 0; i < sbi->nr_ifree_blocks; i++) {
		idx = sbi->nr_istore_blocks + i + 1;

		bh = sb_bread(sb, idx);
		if (!bh)
			return -EIO;

		copy_bitmap_to_le64((__le64 *)bh->b_data,
			(void *)sbi->ifree_bitmap + i * OUICHEFS_BLOCK_SIZE);

		mark_buffer_dirty(bh);
		if (wait)
			sync_dirty_buffer(bh);
		brelse(bh);
	}

	return 0;
}

static int sync_bfree(struct super_block *sb, int wait)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh;
	int i, idx;

	/* Flush free blocks bitmask */
	for (i = 0; i < sbi->nr_bfree_blocks; i++) {
		idx = sbi->nr_istore_blocks + sbi->nr_ifree_blocks + i + 1;

		bh = sb_bread(sb, idx);
		if (!bh)
			return -EIO;

		copy_bitmap_to_le64((__le64 *)bh->b_data,
			(void *)sbi->bfree_bitmap + i * OUICHEFS_BLOCK_SIZE);

		mark_buffer_dirty(bh);
		if (wait)
			sync_dirty_buffer(bh);
		brelse(bh);
	}

	return 0;
}

static void ouichefs_put_super(struct super_block *sb)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	if (sbi) {
		kfree(sbi->ifree_bitmap);
		kfree(sbi->bfree_bitmap);
		kobject_put(&sbi->o_sys->kobj);
		kfree(sbi);
	}
}

static int ouichefs_sync_fs(struct super_block *sb, int wait)
{
	int ret = 0;

	ret = sync_sb_info(sb, wait);
	if (ret)
		return ret;
	ret = sync_ifree(sb, wait);
	if (ret)
		return ret;
	ret = sync_bfree(sb, wait);
	if (ret)
		return ret;

	return 0;
}

static int ouichefs_statfs(struct dentry *dentry, struct kstatfs *stat)
{
	struct super_block *sb = dentry->d_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	stat->f_type = OUICHEFS_MAGIC;
	stat->f_bsize = OUICHEFS_BLOCK_SIZE;
	stat->f_blocks = sbi->nr_blocks;
	stat->f_bfree = sbi->nr_free_blocks;
	stat->f_bavail = sbi->nr_free_blocks;
	stat->f_files = sbi->nr_inodes;
	stat->f_ffree = sbi->nr_free_inodes;
	stat->f_namelen = OUICHEFS_FILENAME_LEN;

	return 0;
}

static struct super_operations ouichefs_super_ops = {
	.put_super = ouichefs_put_super,
	.alloc_inode = ouichefs_alloc_inode,
	.destroy_inode = ouichefs_destroy_inode,
	.write_inode = ouichefs_write_inode,
	.evict_inode = ouichefs_evict_inode,
	.sync_fs = ouichefs_sync_fs,
	.statfs = ouichefs_statfs,
};

static ssize_t free_blocks_show(struct super_block *sb, char *buf)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	return sysfs_emit(buf, "%u\n", sbi->nr_free_blocks);
}

static ssize_t commited_blocks_show(struct super_block *sb, char *buf)
{
	struct inode *inode;
	struct ouichefs_inode_info *ci;
	struct buffer_head *bh_index;
	struct ouichefs_file_index_block *index;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	uint32_t committed_blocks = 0;

	for (uint32_t ino = 0; ino < sbi->nr_inodes; ino++) {
		if (test_bit(ino, sbi->ifree_bitmap))
			continue;

		inode = ouichefs_iget(sb, ino);
		if (IS_ERR(inode))
			return PTR_ERR(inode);

		ci = OUICHEFS_INODE(inode);
		if (ci->index_block)
			committed_blocks++;
		if (!S_ISREG(inode->i_mode)) {
			iput(inode);
			continue;
		}
		bh_index = sb_bread(sb, ci->index_block);
		if (!bh_index) {
			iput(inode);
			return -EIO;
		}

		index = (struct ouichefs_file_index_block *)bh_index->b_data;
		for (uint32_t i = 0; i < index->num_extents; i++) {
			if (!index->extents[i].start)
				continue;
			committed_blocks += index->extents[i].count;
		}

		brelse(bh_index);
		iput(inode);
	}
	return sysfs_emit(buf, "%u\n", committed_blocks);
}

static ssize_t reserved_blocks_show(struct super_block *sb, char *buf)
{
	struct inode *inode;
	struct ouichefs_inode_info *ci;

	uint32_t reserved_blocks = 0;

	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		ci = OUICHEFS_INODE(inode);
		reserved_blocks += ci->i_reserved_count;
	}
	spin_unlock(&sb->s_inode_list_lock);

	return sysfs_emit(buf, "%u\n", reserved_blocks);
}

static ssize_t files_show(struct super_block *sb, char *buf)
{
	struct inode *inode;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	uint32_t num_files = 0;

	for (uint32_t ino = 0; ino < sbi->nr_inodes; ino++) {
		if (test_bit(ino, sbi->ifree_bitmap))
			continue;

		inode = ouichefs_iget(sb, ino);
		if (IS_ERR(inode))
			return PTR_ERR(inode);
		if (S_ISREG(inode->i_mode)) {
			num_files++;
		}
		iput(inode);
	}
	return sysfs_emit(buf, "%u\n", num_files);
}

static ssize_t total_extents_show(struct super_block *sb, char *buf)
{
	struct inode *inode;
	struct ouichefs_inode_info *ci;
	struct buffer_head *bh_index;
	struct ouichefs_file_index_block *index;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	uint64_t total_extents = 0;

	for (uint32_t ino = 0; ino < sbi->nr_inodes; ino++) {
		if (test_bit(ino, sbi->ifree_bitmap))
			continue;

		inode = ouichefs_iget(sb, ino);
		if (IS_ERR(inode))
			return PTR_ERR(inode);
		if (!S_ISREG(inode->i_mode)) {
			iput(inode);
			continue;
		}
		ci = OUICHEFS_INODE(inode);
		bh_index = sb_bread(sb, ci->index_block);
		if (!bh_index) {
			iput(inode);
			return -EIO;
		}

		index = (struct ouichefs_file_index_block *)bh_index->b_data;
		total_extents += index->num_extents;

		brelse(bh_index);
		iput(inode);
	}
	return sysfs_emit(buf, "%llu\n", total_extents);
}

static ssize_t avg_extent_size_show(struct super_block *sb, char *buf)
{
	struct inode *inode;
	struct ouichefs_inode_info *ci;
	struct buffer_head *bh_index;
	struct ouichefs_file_index_block *index;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	uint64_t total_extents = 0;
	uint64_t total_blocks = 0;

	for (uint32_t ino = 0; ino < sbi->nr_inodes; ino++) {
		if (test_bit(ino, sbi->ifree_bitmap))
			continue;

		inode = ouichefs_iget(sb, ino);
		if (IS_ERR(inode))
			return PTR_ERR(inode);
		if (!S_ISREG(inode->i_mode)) {
			iput(inode);
			continue;
		}
		ci = OUICHEFS_INODE(inode);
		bh_index = sb_bread(sb, ci->index_block);
		if (!bh_index) {
			iput(inode);
			return -EIO;
		}

		index = (struct ouichefs_file_index_block *)bh_index->b_data;
		total_extents += index->num_extents;
		for (uint32_t i = 0; i < index->num_extents; i++) {
			if (!index->extents[i].start)
				continue;
			total_blocks += index->extents[i].count;
		}

		brelse(bh_index);
		iput(inode);
	}
	uint32_t avg_extent_size =
		total_extents == 0 ? 0 : (total_blocks * 100) / total_extents;
	return sysfs_emit(buf, "%u\n", avg_extent_size);
}

static ssize_t max_file_size_show(struct super_block *sb, char *buf)
{
	struct inode *inode;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	uint32_t max_inode_size = 0;

	for (uint32_t ino = 0; ino < sbi->nr_inodes; ino++) {
		if (test_bit(ino, sbi->ifree_bitmap))
			continue;
		inode = ouichefs_iget(sb, ino);
		if (IS_ERR(inode))
			return PTR_ERR(inode);

		if (!S_ISREG(inode->i_mode)) {
			iput(inode);
			continue;
		}

		uint32_t size = i_size_read(inode);
		if (size > max_inode_size)
			max_inode_size = size;

		iput(inode);
	}
	return sysfs_emit(buf, "%u\n", max_inode_size);
}

static ssize_t fragmentation_show(struct super_block *sb, char *buf)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct inode *inode;
	struct ouichefs_inode_info *ci;
	struct buffer_head *bh_index;
	struct ouichefs_file_index_block *index;

	uint32_t num_files = 0;
	uint32_t total_extents = 0;

	for (uint32_t ino = 0; ino < sbi->nr_inodes; ino++) {
		if (test_bit(ino, sbi->ifree_bitmap))
			continue;
		inode = ouichefs_iget(sb, ino);
		if (IS_ERR(inode))
			return PTR_ERR(inode);

		if (!S_ISREG(inode->i_mode)) {
			iput(inode);
			continue;
		}

		num_files++;
		ci = OUICHEFS_INODE(inode);
		bh_index = sb_bread(sb, ci->index_block);
		if (!bh_index) {
			iput(inode);
			return -EIO;
		}

		index = (struct ouichefs_file_index_block *)bh_index->b_data;
		total_extents += index->num_extents;
		iput(inode);
	}

	uint32_t fragmentation =
		num_files == 0 ? 0 : (total_extents * 100) / num_files;

	return sysfs_emit(buf, "%u\n", fragmentation);
}

static ssize_t reservation_size_show(struct super_block *sb, char *buf)
{
	return sysfs_emit(buf, "%u\n", reservation_size);
}

static ssize_t gc_runs_show(struct super_block *sb, char *buf)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	return sysfs_emit(buf, "%u\n", sbi->nr_gc_runs);
}

static ssize_t total_blocks_show(struct super_block *sb, char *buf)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	uint32_t total_blocks = sbi->nr_blocks - 1 - sbi->nr_istore_blocks -
				sbi->nr_ifree_blocks - sbi->nr_bfree_blocks;
	return sysfs_emit(buf, "%u\n", total_blocks);
}

static ssize_t reservation_size_store(struct super_block *sb, const char *buf, size_t count)
{
	uint32_t val;
	int rc = sscanf(buf, "%u", &val);
	if (rc != 1 || rc < 0)
		return -EINVAL;

	reservation_size = val;
	return count;
}

struct ouichefs_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct super_block *, char *);
	ssize_t (*store)(struct super_block *, const char *, size_t);
};

static struct ouichefs_sysfs_entry free_blocks_attribute =
	__ATTR(free_blocks, 0400, free_blocks_show, NULL);

static struct ouichefs_sysfs_entry committed_blocks_attribute =
	__ATTR(committed_blocks, 0400, commited_blocks_show, NULL);

static struct ouichefs_sysfs_entry reserved_blocks_attribute =
	__ATTR(reserved_blocks, 0400, reserved_blocks_show, NULL);

static struct ouichefs_sysfs_entry files_attribute =
	__ATTR(files, 0400, files_show, NULL);

static struct ouichefs_sysfs_entry total_extents_attribute =
	__ATTR(total_extents, 0400, total_extents_show, NULL);

static struct ouichefs_sysfs_entry avg_extent_size_attribute =
	__ATTR(avg_extent_size, 0400, avg_extent_size_show, NULL);

static struct ouichefs_sysfs_entry max_file_size_attribute =
	__ATTR(max_file_size, 0400, max_file_size_show, NULL);

static struct ouichefs_sysfs_entry fragmentation_attribute =
	__ATTR(fragmentation, 0400, fragmentation_show, NULL);

static struct ouichefs_sysfs_entry reservation_size_attribute =
	__ATTR(reservation_size, 0644, reservation_size_show, reservation_size_store);

static struct ouichefs_sysfs_entry gc_runs_attribute =
	__ATTR(gc_runs, 0400, gc_runs_show, NULL);

static struct ouichefs_sysfs_entry total_blocks_attribute =
	__ATTR(total_blocks, 0400, total_blocks_show, NULL);

static struct attribute *ouichefs_sys_attrs[] = {
	&free_blocks_attribute.attr,
	&committed_blocks_attribute.attr,
	&reserved_blocks_attribute.attr,
	&files_attribute.attr,
	&total_extents_attribute.attr,
	&avg_extent_size_attribute.attr,
	&max_file_size_attribute.attr,
	&fragmentation_attribute.attr,
	&reservation_size_attribute.attr,
	&gc_runs_attribute.attr,
	&total_blocks_attribute.attr,
	NULL, /* need to NULL terminate the list of attributes */
};
ATTRIBUTE_GROUPS(ouichefs_sys);

static void ouichefs_sys_release(struct kobject *kobj)
{
	struct ouichefs_sysfs *o_sys = to_o_sys(kobj);
	kfree(o_sys);
}

static ssize_t ouichefs_type_show(struct kobject *kobj, struct attribute *attr,
				  char *buf)
{
	struct ouichefs_sysfs *o_sysfs = to_o_sys(kobj);
	struct super_block *sb = o_sysfs->sb;
	struct ouichefs_sysfs_entry *entry;

	entry = container_of(attr, struct ouichefs_sysfs_entry, attr);

	if (!entry->show)
		return -EIO;

	return entry->show(sb, buf);
}

static ssize_t ouichefs_type_store(struct kobject *kobj, struct attribute *attr,
				  const char *buf, size_t count)
{
	struct ouichefs_sysfs *o_sysfs = to_o_sys(kobj);
	struct super_block *sb = o_sysfs->sb;
	struct ouichefs_sysfs_entry *entry;

	entry = container_of(attr, struct ouichefs_sysfs_entry, attr);

	if (!entry->show)
		return -EIO;

	return entry->store(sb, buf, count);
}

static const struct sysfs_ops ouichefs_sysfs_ops = {
	.show = ouichefs_type_show,
	.store = ouichefs_type_store,
};

static struct kobj_type ouichefs_attr_type = {
	.release = ouichefs_sys_release,
	.sysfs_ops = &ouichefs_sysfs_ops,
	.default_groups = ouichefs_sys_groups,
};

/* Fill the struct superblock from partition superblock */
int ouichefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct buffer_head *bh = NULL;
	struct ouichefs_sb_info *csb = NULL;
	struct ouichefs_sb_info *sbi = NULL;
	struct inode *root_inode = NULL;
	struct ouichefs_sysfs *o_sys;
	int ret = 0, i;

	/* Init sb */
	sb->s_magic = OUICHEFS_MAGIC;
	sb_set_blocksize(sb, OUICHEFS_BLOCK_SIZE);
	sb->s_maxbytes = OUICHEFS_MAX_FILESIZE;
	sb->s_op = &ouichefs_super_ops;
	sb->s_time_gran = 1;

	/* Read sb from disk */
	bh = sb_bread(sb, OUICHEFS_SB_BLOCK_NR);
	if (!bh)
		return -EIO;
	csb = (struct ouichefs_sb_info *)bh->b_data;

	/* Check magic number */
	if (le32_to_cpu(csb->magic) != sb->s_magic) {
		pr_err("Wrong magic number\n");
		brelse(bh);
		return -EPERM;
	}

	/* Alloc sb_info */
	sbi = kzalloc(sizeof(struct ouichefs_sb_info), GFP_KERNEL);
	if (!sbi) {
		brelse(bh);
		return -ENOMEM;
	}
	sbi->nr_blocks = le32_to_cpu(csb->nr_blocks);
	sbi->nr_inodes = le32_to_cpu(csb->nr_inodes);
	sbi->nr_istore_blocks = le32_to_cpu(csb->nr_istore_blocks);
	sbi->nr_ifree_blocks = le32_to_cpu(csb->nr_ifree_blocks);
	sbi->nr_bfree_blocks = le32_to_cpu(csb->nr_bfree_blocks);
	sbi->nr_free_inodes = le32_to_cpu(csb->nr_free_inodes);
	sbi->nr_free_blocks = le32_to_cpu(csb->nr_free_blocks);

	o_sys = kzalloc(sizeof(*o_sys), GFP_KERNEL);
	if (!o_sys) {
		return -ENOMEM;
	}
	kobject_init(&o_sys->kobj, &ouichefs_attr_type);
	o_sys->sb = sb;
	sbi->o_sys = o_sys;
	ret = kobject_add(&o_sys->kobj, ouichefs_kobj,
			  sb->s_bdev->bd_disk->disk_name);
	if (ret) {
		kobject_put(&o_sys->kobj);
		kfree(o_sys);
		return ret;
	}
	sb->s_fs_info = sbi;

	brelse(bh);

	/* Alloc and copy ifree_bitmap */
	sbi->ifree_bitmap =
		kzalloc(sbi->nr_ifree_blocks * OUICHEFS_BLOCK_SIZE, GFP_KERNEL);
	if (!sbi->ifree_bitmap) {
		ret = -ENOMEM;
		goto free_sbi;
	}
	for (i = 0; i < sbi->nr_ifree_blocks; i++) {
		int idx = sbi->nr_istore_blocks + i + 1;

		bh = sb_bread(sb, idx);
		if (!bh) {
			ret = -EIO;
			goto free_ifree;
		}

		copy_bitmap_from_le64((void *)sbi->ifree_bitmap +
					      i * OUICHEFS_BLOCK_SIZE,
				      (__le64 *)bh->b_data);

		brelse(bh);
	}

	/* Alloc and copy bfree_bitmap */
	sbi->bfree_bitmap =
		kzalloc(sbi->nr_bfree_blocks * OUICHEFS_BLOCK_SIZE, GFP_KERNEL);
	if (!sbi->bfree_bitmap) {
		ret = -ENOMEM;
		goto free_ifree;
	}
	for (i = 0; i < sbi->nr_bfree_blocks; i++) {
		int idx = sbi->nr_istore_blocks + sbi->nr_ifree_blocks + i + 1;

		bh = sb_bread(sb, idx);
		if (!bh) {
			ret = -EIO;
			goto free_bfree;
		}

		copy_bitmap_from_le64((void *)sbi->bfree_bitmap +
					      i * OUICHEFS_BLOCK_SIZE,
				      (__le64 *)bh->b_data);

		brelse(bh);
	}

	/* 
	 * Create root inode.
	 *
	 * 1 is used instead of 0 to stay compatible with userspace applications,
	 * as this is the "de facto standard".
	 *
	 * See:
	 * - https://github.com/rgouicem/ouichefs/commit/296e162
	 * - https://github.com/rgouicem/ouichefs/pull/23
	 */
	root_inode = ouichefs_iget(sb, 1);
	if (IS_ERR(root_inode)) {
		ret = PTR_ERR(root_inode);
		goto free_bfree;
	}
	inode_init_owner(&nop_mnt_idmap, root_inode, NULL, root_inode->i_mode);
	/* d_make_root should only be run once */
	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto free_bfree;
	}

	return 0;

free_bfree:
	kfree(sbi->bfree_bitmap);
free_ifree:
	kfree(sbi->ifree_bitmap);
free_sbi:
	kfree(sbi);

	return ret;
}
