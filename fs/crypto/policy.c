// SPDX-License-Identifier: GPL-2.0
/*
 * Encryption policy functions for per-file encryption support.
 *
 * Copyright (C) 2015, Google, Inc.
 * Copyright (C) 2015, Motorola Mobility.
 *
 * Written by Michael Halcrow, 2015.
 * Modified by Jaegeuk Kim, 2015.
 */

#include <linux/random.h>
#include <linux/string.h>
#include <linux/mount.h>
#include <linux/hie.h>
#include "fscrypt_private.h"

/*
 * check whether an encryption policy is consistent with an encryption context
 */
static bool is_encryption_context_consistent_with_policy(
				const struct fscrypt_context *ctx,
				const struct fscrypt_policy *policy,
				const struct inode *inode)
{

	if ((ctx->contents_encryption_mode !=
		 policy->contents_encryption_mode) &&
		!(hie_is_capable(inode->i_sb) &&
		 (ctx->contents_encryption_mode ==
		 FS_ENCRYPTION_MODE_PRIVATE)))
		return 0;

	return memcmp(ctx->master_key_descriptor, policy->master_key_descriptor,
		      FS_KEY_DESCRIPTOR_SIZE) == 0 &&
		(ctx->flags == policy->flags) &&
		(ctx->filenames_encryption_mode ==
		 policy->filenames_encryption_mode);
}

static int create_encryption_context_from_policy(struct inode *inode,
				const struct fscrypt_policy *policy)
{
	struct fscrypt_context ctx;

	ctx.format = FS_ENCRYPTION_CONTEXT_FORMAT_V1;
	memcpy(ctx.master_key_descriptor, policy->master_key_descriptor,
					FS_KEY_DESCRIPTOR_SIZE);

	if (!fscrypt_valid_enc_modes(policy->contents_encryption_mode,
				     policy->filenames_encryption_mode))
		return -EINVAL;

	if (policy->flags & ~FS_POLICY_FLAGS_VALID)
		return -EINVAL;

	if ((policy->flags & FS_POLICY_FLAG_IV_INO_LBLK_32) &&
	    policy->contents_encryption_mode != FS_ENCRYPTION_MODE_PRIVATE)
		return -EINVAL;

	ctx.contents_encryption_mode =
		fscrypt_data_crypt_mode(inode,
		policy->contents_encryption_mode);
	ctx.filenames_encryption_mode = policy->filenames_encryption_mode;
	ctx.flags = policy->flags;
	BUILD_BUG_ON(sizeof(ctx.nonce) != FS_KEY_DERIVATION_NONCE_SIZE);
	get_random_bytes(ctx.nonce, FS_KEY_DERIVATION_NONCE_SIZE);

	return inode->i_sb->s_cop->set_context(inode, &ctx, sizeof(ctx), NULL);
}

int fscrypt_ioctl_set_policy(struct file *filp, const void __user *arg)
{
	struct fscrypt_policy policy;
	struct inode *inode = file_inode(filp);
	int ret;
	struct fscrypt_context ctx;

	if (copy_from_user(&policy, arg, sizeof(policy)))
		return -EFAULT;

	if (!inode_owner_or_capable(inode))
		return -EACCES;

	if (policy.version != 0)
		return -EINVAL;

	ret = mnt_want_write_file(filp);
	if (ret)
		return ret;

	inode_lock(inode);

	ret = inode->i_sb->s_cop->get_context(inode, &ctx, sizeof(ctx));
	if (ret == -ENODATA) {
		if (!S_ISDIR(inode->i_mode))
			ret = -ENOTDIR;
		else if (IS_DEADDIR(inode))
			ret = -ENOENT;
		else if (!inode->i_sb->s_cop->empty_dir(inode))
			ret = -ENOTEMPTY;
		else
			ret = create_encryption_context_from_policy(inode,
								    &policy);
	} else if (ret == sizeof(ctx) &&
		   is_encryption_context_consistent_with_policy(&ctx,
								&policy,
								inode)) {
		/* The file already uses the same encryption policy. */
		ret = 0;
	} else if (ret >= 0 || ret == -ERANGE) {
		/* The file already uses a different encryption policy. */
		ret = -EEXIST;
	}

	inode_unlock(inode);

	mnt_drop_write_file(filp);
	return ret;
}
EXPORT_SYMBOL(fscrypt_ioctl_set_policy);

int fscrypt_ioctl_get_policy(struct file *filp, void __user *arg)
{
	struct inode *inode = file_inode(filp);
	struct fscrypt_context ctx;
	struct fscrypt_policy policy;
	int res;

	if (!IS_ENCRYPTED(inode))
		return -ENODATA;

	res = inode->i_sb->s_cop->get_context(inode, &ctx, sizeof(ctx));
	if (res < 0 && res != -ERANGE)
		return res;
	if (res != sizeof(ctx))
		return -EINVAL;
	if (ctx.format != FS_ENCRYPTION_CONTEXT_FORMAT_V1)
		return -EINVAL;

	policy.version = 0;
	policy.contents_encryption_mode = ctx.contents_encryption_mode;
	policy.filenames_encryption_mode = ctx.filenames_encryption_mode;
	policy.flags = ctx.flags;

	/* in compliance with android */
	if (S_ISDIR(inode->i_mode) &&
		policy.contents_encryption_mode !=
		FS_ENCRYPTION_MODE_INVALID)
		policy.contents_encryption_mode =
			FS_ENCRYPTION_MODE_AES_256_XTS;

	memcpy(policy.master_key_descriptor, ctx.master_key_descriptor,
				FS_KEY_DESCRIPTOR_SIZE);

	if (copy_to_user(arg, &policy, sizeof(policy)))
		return -EFAULT;
	return 0;
}
EXPORT_SYMBOL(fscrypt_ioctl_get_policy);

/**
 * fscrypt_has_permitted_context() - is a file's encryption policy permitted
 *				     within its directory?
 *
 * @parent: inode for parent directory
 * @child: inode for file being looked up, opened, or linked into @parent
 *
 * Filesystems must call this before permitting access to an inode in a
 * situation where the parent directory is encrypted (either before allowing
 * ->lookup() to succeed, or for a regular file before allowing it to be opened)
 * and before any operation that involves linking an inode into an encrypted
 * directory, including link, rename, and cross rename.  It enforces the
 * constraint that within a given encrypted directory tree, all files use the
 * same encryption policy.  The pre-access check is needed to detect potentially
 * malicious offline violations of this constraint, while the link and rename
 * checks are needed to prevent online violations of this constraint.
 *
 * Return: 1 if permitted, 0 if forbidden.
 */
int fscrypt_has_permitted_context(struct inode *parent, struct inode *child)
{
	const struct fscrypt_operations *cops = parent->i_sb->s_cop;
	const struct fscrypt_info *parent_ci, *child_ci;
	struct fscrypt_context parent_ctx, child_ctx;
	int res;

	/* No restrictions on file types which are never encrypted */
	if (!S_ISREG(child->i_mode) && !S_ISDIR(child->i_mode) &&
	    !S_ISLNK(child->i_mode))
		return 1;

	/* No restrictions if the parent directory is unencrypted */
	if (!IS_ENCRYPTED(parent))
		return 1;

	/* Encrypted directories must not contain unencrypted files */
	if (!IS_ENCRYPTED(child))
		return 0;

	/*
	 * Both parent and child are encrypted, so verify they use the same
	 * encryption policy.  Compare the fscrypt_info structs if the keys are
	 * available, otherwise retrieve and compare the fscrypt_contexts.
	 *
	 * Note that the fscrypt_context retrieval will be required frequently
	 * when accessing an encrypted directory tree without the key.
	 * Performance-wise this is not a big deal because we already don't
	 * really optimize for file access without the key (to the extent that
	 * such access is even possible), given that any attempted access
	 * already causes a fscrypt_context retrieval and keyring search.
	 *
	 * In any case, if an unexpected error occurs, fall back to "forbidden".
	 */

	res = fscrypt_get_encryption_info(parent);
	if (res)
		return 0;
	res = fscrypt_get_encryption_info(child);
	if (res)
		return 0;
	parent_ci = parent->i_crypt_info;
	child_ci = child->i_crypt_info;

	if (parent_ci && child_ci) {
		return memcmp(parent_ci->ci_master_key_descriptor,
			      child_ci->ci_master_key_descriptor,
			      FS_KEY_DESCRIPTOR_SIZE) == 0 &&
			(parent_ci->ci_data_mode == child_ci->ci_data_mode) &&
			(parent_ci->ci_filename_mode ==
			 child_ci->ci_filename_mode) &&
			//(parent_ci->ci_flags == child_ci->ci_flags);
		//MTK PATCH: f2fs+emmc hwmcdq new file use new iv.
			((parent_ci->ci_flags &
			  ~FS_POLICY_FLAG_IV_INO_LBLK_32) ==
			 (child_ci->ci_flags &
			  ~FS_POLICY_FLAG_IV_INO_LBLK_32));
	}

	res = cops->get_context(parent, &parent_ctx, sizeof(parent_ctx));
	if (res != sizeof(parent_ctx))
		return 0;

	res = cops->get_context(child, &child_ctx, sizeof(child_ctx));
	if (res != sizeof(child_ctx))
		return 0;

	parent_ctx.contents_encryption_mode =
		fscrypt_data_crypt_mode(parent,
		parent_ctx.contents_encryption_mode);
	child_ctx.contents_encryption_mode =
		fscrypt_data_crypt_mode(child,
		child_ctx.contents_encryption_mode);

	return memcmp(parent_ctx.master_key_descriptor,
		      child_ctx.master_key_descriptor,
		      FS_KEY_DESCRIPTOR_SIZE) == 0 &&
		(parent_ctx.contents_encryption_mode ==
		 child_ctx.contents_encryption_mode) &&
		(parent_ctx.filenames_encryption_mode ==
		 child_ctx.filenames_encryption_mode) &&
		//(parent_ctx.flags == child_ctx.flags);
		//MTK PATCH:
		((parent_ctx.flags & ~FS_POLICY_FLAG_IV_INO_LBLK_32) ==
		 (child_ctx.flags & ~FS_POLICY_FLAG_IV_INO_LBLK_32));
}
EXPORT_SYMBOL(fscrypt_has_permitted_context);

#define BOOTDEV_SDMMC           (1)
#define BOOTDEV_UFS             (2)
bool fscrypt_force_iv_ino_lblk_32(void)
{
#ifdef CONFIG_MTK_EMMC_HW_CQ
	return  get_boot_type() == BOOTDEV_SDMMC;
#else
	return	false;
#endif
}

/**
 * fscrypt_inherit_context() - Sets a child context from its parent
 * @parent: Parent inode from which the context is inherited.
 * @child:  Child inode that inherits the context from @parent.
 * @fs_data:  private data given by FS.
 * @preload:  preload child i_crypt_info if true
 *
 * Return: 0 on success, -errno on failure
 */
int fscrypt_inherit_context(struct inode *parent, struct inode *child,
						void *fs_data, bool preload)
{
	struct fscrypt_context ctx;
	struct fscrypt_info *ci;
	int res;

	res = fscrypt_get_encryption_info(parent);
	if (res < 0)
		return res;

	ci = parent->i_crypt_info;
	if (ci == NULL)
		return -ENOKEY;

	ctx.format = FS_ENCRYPTION_CONTEXT_FORMAT_V1;
	ctx.contents_encryption_mode = ci->ci_data_mode;
	ctx.filenames_encryption_mode = ci->ci_filename_mode;
	ctx.flags = ci->ci_flags;

	//only for emmc device, add FS_POLICY_FLAG_IV_INO_LBLK_32
	if (ctx.contents_encryption_mode == FS_ENCRYPTION_MODE_PRIVATE
			&& fscrypt_force_iv_ino_lblk_32()) {
		ctx.flags |= FS_POLICY_FLAG_IV_INO_LBLK_32;
	}

	memcpy(ctx.master_key_descriptor, ci->ci_master_key_descriptor,
	       FS_KEY_DESCRIPTOR_SIZE);
	get_random_bytes(ctx.nonce, FS_KEY_DERIVATION_NONCE_SIZE);
	BUILD_BUG_ON(sizeof(ctx) != FSCRYPT_SET_CONTEXT_MAX_SIZE);
	res = parent->i_sb->s_cop->set_context(child, &ctx,
						sizeof(ctx), fs_data);
	if (res)
		return res;
	return preload ? fscrypt_get_encryption_info(child): 0;
}
EXPORT_SYMBOL(fscrypt_inherit_context);

int fscrypt_set_bio_ctx(struct inode *inode, struct bio *bio)
{
	struct fscrypt_info *ci;
	int ret = -ENOENT;

	if (!inode || !bio)
		return ret;

	ci = inode->i_crypt_info;

	if (S_ISREG(inode->i_mode) && ci &&
	    (ci->ci_data_mode == FS_ENCRYPTION_MODE_PRIVATE)) {
		WARN_ON(!hie_is_capable(inode->i_sb));
		/* HIE: default use aes-256-xts */
		bio_bcf_set(bio, BC_CRYPT | BC_AES_256_XTS);
		bio->bi_crypt_ctx.bc_key_size = FS_AES_256_XTS_KEY_SIZE;
		bio->bi_crypt_ctx.bc_ino = inode->i_ino;
		bio->bi_crypt_ctx.bc_sb = inode->i_sb;
		bio->bi_crypt_ctx.bc_info_act = &fscrypt_crypt_info_act;
		bio->bi_crypt_ctx.bc_info =
			fscrypt_crypt_info_act(
			ci, BIO_BC_INFO_GET);
		bio->bi_crypt_ctx.hashed_info = ci->ci_hashed_info;

		WARN_ON(!bio->bi_crypt_ctx.bc_info);

#ifdef CONFIG_HIE_DEBUG
		if (hie_debug(HIE_DBG_FS))
			pr_info("HIE: %s: ino: %ld, bio: %p\n",
				__func__, inode->i_ino, bio);
#endif
		ret = 0;
	} else
		bio_bcf_clear(bio, BC_CRYPT);

	return ret;
}

int fscrypt_key_payload(struct bio_crypt_ctx *ctx,
		const unsigned char **key)
{
	struct fscrypt_info *fi;

	fi = (struct fscrypt_info *)ctx->bc_info;

	if (!fi) {
		pr_info("HIE: %s: missing crypto info\n", __func__);
		return -ENOKEY;
	}

	if (key)
		*key = &(fi->ci_raw_key[0]);

	return ctx->bc_key_size;
}

int fscrypt_is_hw_encrypt(const struct inode *inode)
{
	struct fscrypt_info *ci = inode->i_crypt_info;

	return S_ISREG(inode->i_mode) && ci &&
		ci->ci_data_mode == FS_ENCRYPTION_MODE_PRIVATE;
}

int fscrypt_is_sw_encrypt(const struct inode *inode)
{
	struct fscrypt_info *ci = inode->i_crypt_info;

	return S_ISREG(inode->i_mode) && ci &&
		ci->ci_data_mode != FS_ENCRYPTION_MODE_INVALID &&
		ci->ci_data_mode != FS_ENCRYPTION_MODE_PRIVATE;
}
