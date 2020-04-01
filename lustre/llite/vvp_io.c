/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * Implementation of cl_io for VVP layer.
 *
 *   Author: Nikita Danilov <nikita.danilov@sun.com>
 *   Author: Jinshan Xiong <jinshan.xiong@whamcloud.com>
 */

#define DEBUG_SUBSYSTEM S_LLITE


#include <obd.h>
#include <linux/pagevec.h>
#include <linux/memcontrol.h>
#include "llite_internal.h"
#include "vvp_internal.h"
#include <libcfs/linux/linux-misc.h>

static struct vvp_io *cl2vvp_io(const struct lu_env *env,
				const struct cl_io_slice *slice)
{
	struct vvp_io *vio;

	vio = container_of(slice, struct vvp_io, vui_cl);
	LASSERT(vio == vvp_env_io(env));

	return vio;
}

/**
 * True, if \a io is a normal io, False for splice_{read,write}
 */
static int cl_is_normalio(const struct lu_env *env, const struct cl_io *io)
{
	struct vvp_io *vio = vvp_env_io(env);

	LASSERT(io->ci_type == CIT_READ || io->ci_type == CIT_WRITE);

	return vio->vui_io_subtype == IO_NORMAL;
}

/**
 * For swapping layout. The file's layout may have changed.
 * To avoid populating pages to a wrong stripe, we have to verify the
 * correctness of layout. It works because swapping layout processes
 * have to acquire group lock.
 */
static bool can_populate_pages(const struct lu_env *env, struct cl_io *io,
				struct inode *inode)
{
	struct ll_inode_info	*lli = ll_i2info(inode);
	struct vvp_io		*vio = vvp_env_io(env);
	bool rc = true;

	switch (io->ci_type) {
	case CIT_READ:
	case CIT_WRITE:
		/* don't need lock here to check lli_layout_gen as we have held
		 * extent lock and GROUP lock has to hold to swap layout */
		if (ll_layout_version_get(lli) != vio->vui_layout_gen ||
		    OBD_FAIL_CHECK_RESET(OBD_FAIL_LLITE_LOST_LAYOUT, 0)) {
			io->ci_need_restart = 1;
			/* this will cause a short read/write */
			io->ci_continue = 0;
			rc = false;
		}
	case CIT_FAULT:
		/* fault is okay because we've already had a page. */
	default:
		break;
	}

	return rc;
}

static void vvp_object_size_lock(struct cl_object *obj)
{
	struct inode *inode = vvp_object_inode(obj);

	ll_inode_size_lock(inode);
	cl_object_attr_lock(obj);
}

static void vvp_object_size_unlock(struct cl_object *obj)
{
	struct inode *inode = vvp_object_inode(obj);

	cl_object_attr_unlock(obj);
	ll_inode_size_unlock(inode);
}

/**
 * Helper function that if necessary adjusts file size (inode->i_size), when
 * position at the offset \a pos is accessed. File size can be arbitrary stale
 * on a Lustre client, but client at least knows KMS. If accessed area is
 * inside [0, KMS], set file size to KMS, otherwise glimpse file size.
 *
 * Locking: i_size_lock is used to serialize changes to inode size and to
 * protect consistency between inode size and cl_object
 * attributes. cl_object_size_lock() protects consistency between cl_attr's of
 * top-object and sub-objects.
 */
static int vvp_prep_size(const struct lu_env *env, struct cl_object *obj,
			 struct cl_io *io, loff_t start, size_t count,
			 int *exceed)
{
	struct cl_attr *attr  = vvp_env_thread_attr(env);
	struct inode   *inode = vvp_object_inode(obj);
	loff_t          pos   = start + count - 1;
	loff_t kms;
	int result;

	/*
	 * Consistency guarantees: following possibilities exist for the
	 * relation between region being accessed and real file size at this
	 * moment:
	 *
	 *  (A): the region is completely inside of the file;
	 *
	 *  (B-x): x bytes of region are inside of the file, the rest is
	 *  outside;
	 *
	 *  (C): the region is completely outside of the file.
	 *
	 * This classification is stable under DLM lock already acquired by
	 * the caller, because to change the class, other client has to take
	 * DLM lock conflicting with our lock. Also, any updates to ->i_size
	 * by other threads on this client are serialized by
	 * ll_inode_size_lock(). This guarantees that short reads are handled
	 * correctly in the face of concurrent writes and truncates.
	 */
	vvp_object_size_lock(obj);
	result = cl_object_attr_get(env, obj, attr);
	if (result == 0) {
		kms = attr->cat_kms;
		if (pos > kms) {
			/*
			 * A glimpse is necessary to determine whether we
			 * return a short read (B) or some zeroes at the end
			 * of the buffer (C)
			 */
			vvp_object_size_unlock(obj);
			result = cl_glimpse_lock(env, io, inode, obj, 0);
			if (result == 0 && exceed != NULL) {
				/* If objective page index exceed end-of-file
				 * page index, return directly. Do not expect
				 * kernel will check such case correctly.
				 * linux-2.6.18-128.1.1 miss to do that.
				 * --bug 17336 */
				loff_t size = i_size_read(inode);
				unsigned long cur_index = start >>
					PAGE_SHIFT;

				if ((size == 0 && cur_index != 0) ||
				    (((size - 1) >> PAGE_SHIFT) <
				     cur_index))
					*exceed = 1;
			}

			return result;
		} else {
			/*
			 * region is within kms and, hence, within real file
			 * size (A). We need to increase i_size to cover the
			 * read region so that generic_file_read() will do its
			 * job, but that doesn't mean the kms size is
			 * _correct_, it is only the _minimum_ size. If
			 * someone does a stat they will get the correct size
			 * which will always be >= the kms value here.
			 * b=11081
			 */
			if (i_size_read(inode) < kms) {
				i_size_write(inode, kms);
				CDEBUG(D_VFSTRACE,
				       DFID" updating i_size %llu\n",
				       PFID(lu_object_fid(&obj->co_lu)),
				       (__u64)i_size_read(inode));
			}
		}
	}

	vvp_object_size_unlock(obj);

	return result;
}

/*****************************************************************************
 *
 * io operations.
 *
 */

static int vvp_io_one_lock_index(const struct lu_env *env, struct cl_io *io,
				 __u32 enqflags, enum cl_lock_mode mode,
				 pgoff_t start, pgoff_t end)
{
	struct vvp_io          *vio   = vvp_env_io(env);
	struct cl_lock_descr   *descr = &vio->vui_link.cill_descr;
	struct cl_object       *obj   = io->ci_obj;

	CLOBINVRNT(env, obj, vvp_object_invariant(obj));
	ENTRY;

	CDEBUG(D_VFSTRACE, "lock: %d [%lu, %lu]\n", mode, start, end);

	memset(&vio->vui_link, 0, sizeof vio->vui_link);

	if (vio->vui_fd && (vio->vui_fd->fd_flags & LL_FILE_GROUP_LOCKED)) {
		descr->cld_mode = CLM_GROUP;
		descr->cld_gid  = vio->vui_fd->fd_grouplock.lg_gid;
		enqflags |= CEF_LOCK_MATCH;
	} else {
		descr->cld_mode  = mode;
	}

	descr->cld_obj   = obj;
	descr->cld_start = start;
	descr->cld_end   = end;
	descr->cld_enq_flags = enqflags;

	cl_io_lock_add(env, io, &vio->vui_link);

	RETURN(0);
}

static int vvp_io_one_lock(const struct lu_env *env, struct cl_io *io,
			   __u32 enqflags, enum cl_lock_mode mode,
			   loff_t start, loff_t end)
{
	struct cl_object *obj = io->ci_obj;

	return vvp_io_one_lock_index(env, io, enqflags, mode,
				     cl_index(obj, start), cl_index(obj, end));
}

static int vvp_io_write_iter_init(const struct lu_env *env,
				  const struct cl_io_slice *ios)
{
	struct vvp_io *vio = cl2vvp_io(env, ios);

	cl_page_list_init(&vio->u.write.vui_queue);
	vio->u.write.vui_written = 0;
	vio->u.write.vui_from = 0;
	vio->u.write.vui_to = PAGE_SIZE;

	return 0;
}

static void vvp_io_write_iter_fini(const struct lu_env *env,
				   const struct cl_io_slice *ios)
{
	struct vvp_io *vio = cl2vvp_io(env, ios);

	LASSERT(vio->u.write.vui_queue.pl_nr == 0);
}

static int vvp_io_fault_iter_init(const struct lu_env *env,
                                  const struct cl_io_slice *ios)
{
	struct vvp_io *vio   = cl2vvp_io(env, ios);
	struct inode  *inode = vvp_object_inode(ios->cis_obj);

	LASSERT(inode == file_inode(vio->vui_fd->fd_file));
	vio->u.fault.ft_mtime = inode->i_mtime.tv_sec;

	return 0;
}

static void vvp_io_fini(const struct lu_env *env, const struct cl_io_slice *ios)
{
	struct cl_io     *io  = ios->cis_io;
	struct cl_object *obj = io->ci_obj;
	struct vvp_io    *vio = cl2vvp_io(env, ios);
	struct inode     *inode = vvp_object_inode(obj);
	__u32		  gen = 0;
	int retries = 0;
	int rc;
	ENTRY;

	CLOBINVRNT(env, obj, vvp_object_invariant(obj));

	CDEBUG(D_VFSTRACE, DFID" ignore/verify layout %d/%d, layout version %d "
			   "need write layout %d, restore needed %d\n",
	       PFID(lu_object_fid(&obj->co_lu)),
	       io->ci_ignore_layout, io->ci_verify_layout,
	       vio->vui_layout_gen, io->ci_need_write_intent,
	       io->ci_restore_needed);

	while (io->ci_restore_needed) {
		/* file was detected release, we need to restore it
		 * before finishing the io
		 */
		rc = ll_layout_restore(inode, 0, OBD_OBJECT_EOF);
		/*
		 * if restore registration failed, or if the
		 * CDT_NONBLOCKING_RESTORE flag is set, -ENODATA will be
		 * returned and we need to exit immediately
		 *
		 * The layout will change after restore, so we need to
		 * block on layout lock held by the MDT
		 * as MDT will not send new layout in lvb (see LU-3124)
		 * we have to explicitly fetch it, all this will be done
		 * by ll_layout_refresh().
		 *
		 * Even if ll_layout_restore() returns zero, it doesn't mean
		 * that restore has been successful. Therefore it sets
		 * ci_verify_layout so that it will check layout at the end
		 * of this function.
		 */
		if (retries++ >= 10 || rc) {
			io->ci_restore_needed = 1;
			io->ci_need_restart = 0;
			io->ci_verify_layout = 0;
			io->ci_result = rc;
			GOTO(out, rc);
		}

		/* Even if ll_layout_restore() returns zero, it doesn't mean
		 * that restore has been successful. Therefore it should verify
		 * if there was layout change and restart I/O correspondingly.
		 */
		ll_layout_refresh(inode, &gen);
		io->ci_need_restart = vio->vui_layout_gen != gen;
		/* Successful restore is indicate by the layout gen changing */
		if (io->ci_need_restart) {
			CDEBUG(D_VFSTRACE,
			       DFID" layout changed from %d to %d.\n",
			       PFID(lu_object_fid(&obj->co_lu)),
			       vio->vui_layout_gen, gen);
			/* restore was done, clear restoring state */
			io->ci_restore_needed = 0;
			ll_file_clear_flag(ll_i2info(vvp_object_inode(obj)),
					   LLIF_FILE_RESTORING);
			GOTO(out, 0);
		}
	}

	/**
	 * dynamic layout change needed, send layout intent
	 * RPC.
	 */
	if (io->ci_need_write_intent) {
		enum layout_intent_opc opc = LAYOUT_INTENT_WRITE;

		io->ci_need_write_intent = 0;

		LASSERT(io->ci_type == CIT_WRITE ||
			cl_io_is_trunc(io) || cl_io_is_mkwrite(io));

		CDEBUG(D_VFSTRACE, DFID" write layout, type %u "DEXT"\n",
		       PFID(lu_object_fid(&obj->co_lu)), io->ci_type,
		       PEXT(&io->ci_write_intent));

		if (cl_io_is_trunc(io))
			opc = LAYOUT_INTENT_TRUNC;

		rc = ll_layout_write_intent(inode, opc, &io->ci_write_intent);
		io->ci_result = rc;
		if (!rc)
			io->ci_need_restart = 1;
		GOTO(out, rc);
	}

	if (!io->ci_need_restart &&
	    !io->ci_ignore_layout && io->ci_verify_layout) {
		/* check layout version */
		ll_layout_refresh(inode, &gen);
		io->ci_need_restart = vio->vui_layout_gen != gen;
		if (io->ci_need_restart) {
			CDEBUG(D_VFSTRACE,
			       DFID" layout changed from %d to %d.\n",
			       PFID(lu_object_fid(&obj->co_lu)),
			       vio->vui_layout_gen, gen);
		}
		GOTO(out, 0);
	}
out:
	EXIT;
}

static void vvp_io_fault_fini(const struct lu_env *env,
                              const struct cl_io_slice *ios)
{
        struct cl_io   *io   = ios->cis_io;
        struct cl_page *page = io->u.ci_fault.ft_page;

	CLOBINVRNT(env, io->ci_obj, vvp_object_invariant(io->ci_obj));

        if (page != NULL) {
                lu_ref_del(&page->cp_reference, "fault", io);
                cl_page_put(env, page);
                io->u.ci_fault.ft_page = NULL;
        }
        vvp_io_fini(env, ios);
}

static enum cl_lock_mode vvp_mode_from_vma(struct vm_area_struct *vma)
{
        /*
         * we only want to hold PW locks if the mmap() can generate
         * writes back to the file and that only happens in shared
         * writable vmas
         */
        if ((vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_WRITE))
                return CLM_WRITE;
        return CLM_READ;
}

static int vvp_mmap_locks(const struct lu_env *env,
			  struct vvp_io *vio, struct cl_io *io)
{
	struct vvp_thread_info *vti = vvp_env_info(env);
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	struct cl_lock_descr *descr = &vti->vti_descr;
	union ldlm_policy_data policy;
	struct iovec iov;
	struct iov_iter i;
	unsigned long addr;
	ssize_t count;
	int result = 0;
	ENTRY;

	LASSERT(io->ci_type == CIT_READ || io->ci_type == CIT_WRITE);

	if (!cl_is_normalio(env, io))
		RETURN(0);

	/* nfs or loop back device write */
	if (vio->vui_iter == NULL)
		RETURN(0);

	/* No MM (e.g. NFS)? No vmas too. */
	if (mm == NULL)
		RETURN(0);

	if (!iter_is_iovec(vio->vui_iter) && !iov_iter_is_kvec(vio->vui_iter))
		RETURN(0);

	for (i = *vio->vui_iter;
	     iov_iter_count(&i);
	     iov_iter_advance(&i, iov.iov_len)) {
		iov = iov_iter_iovec(&i);
		addr = (unsigned long)iov.iov_base;
		count = iov.iov_len;

                if (count == 0)
                        continue;

		count += addr & ~PAGE_MASK;
		addr &= PAGE_MASK;

                down_read(&mm->mmap_sem);
                while((vma = our_vma(mm, addr, count)) != NULL) {
			struct dentry *de = file_dentry(vma->vm_file);
			struct inode *inode = de->d_inode;
                        int flags = CEF_MUST;

			if (ll_file_nolock(vma->vm_file)) {
				/*
				 * For no lock case is not allowed for mmap
				 */
				result = -EINVAL;
				break;
			}

                        /*
                         * XXX: Required lock mode can be weakened: CIT_WRITE
                         * io only ever reads user level buffer, and CIT_READ
                         * only writes on it.
                         */
                        policy_from_vma(&policy, vma, addr, count);
                        descr->cld_mode = vvp_mode_from_vma(vma);
                        descr->cld_obj = ll_i2info(inode)->lli_clob;
                        descr->cld_start = cl_index(descr->cld_obj,
                                                    policy.l_extent.start);
                        descr->cld_end = cl_index(descr->cld_obj,
                                                  policy.l_extent.end);
                        descr->cld_enq_flags = flags;
                        result = cl_io_lock_alloc_add(env, io, descr);

                        CDEBUG(D_VFSTRACE, "lock: %d: [%lu, %lu]\n",
                               descr->cld_mode, descr->cld_start,
                               descr->cld_end);

			if (result < 0)
				break;

			if (vma->vm_end - addr >= count)
				break;

			count -= vma->vm_end - addr;
			addr = vma->vm_end;
		}
		up_read(&mm->mmap_sem);
		if (result < 0)
			break;
	}
	RETURN(result);
}

static void vvp_io_advance(const struct lu_env *env,
			   const struct cl_io_slice *ios,
			   size_t nob)
{
	struct vvp_io    *vio = cl2vvp_io(env, ios);
	struct cl_io     *io  = ios->cis_io;
	struct cl_object *obj = ios->cis_io->ci_obj;

	CLOBINVRNT(env, obj, vvp_object_invariant(obj));

	if (!cl_is_normalio(env, io))
		return;

	vio->vui_tot_count -= nob;
	iov_iter_reexpand(vio->vui_iter, vio->vui_tot_count);
}

static void vvp_io_update_iov(const struct lu_env *env,
			      struct vvp_io *vio, struct cl_io *io)
{
	size_t size = io->u.ci_rw.crw_count;

	if (!cl_is_normalio(env, io) || vio->vui_iter == NULL)
		return;

	iov_iter_truncate(vio->vui_iter, size);
}

static int vvp_io_rw_lock(const struct lu_env *env, struct cl_io *io,
                          enum cl_lock_mode mode, loff_t start, loff_t end)
{
	struct vvp_io *vio = vvp_env_io(env);
	struct cl_object	*obj   = io->ci_obj;
	struct inode		*inode = vvp_object_inode(obj);
	struct ll_inode_info	*lli = ll_i2info(inode);
	int result;
	int ast_flags = 0;

	LASSERT(io->ci_type == CIT_READ || io->ci_type == CIT_WRITE);
	ENTRY;

	if (vio->vui_io_subtype == IO_NORMAL)
		down_read(&lli->lli_trunc_sem);

	vvp_io_update_iov(env, vio, io);

	if (io->u.ci_rw.crw_nonblock)
		ast_flags |= CEF_NONBLOCK;
	if (io->ci_lock_no_expand)
		ast_flags |= CEF_LOCK_NO_EXPAND;

	result = vvp_mmap_locks(env, vio, io);
	if (result == 0)
		result = vvp_io_one_lock(env, io, ast_flags, mode, start, end);

	RETURN(result);
}

static int vvp_io_read_lock(const struct lu_env *env,
                            const struct cl_io_slice *ios)
{
	struct cl_io *io = ios->cis_io;
	struct cl_io_rw_common *rd = &io->u.ci_rd.rd;
	int result;

	ENTRY;
	result = vvp_io_rw_lock(env, io, CLM_READ, rd->crw_pos,
				rd->crw_pos + rd->crw_count - 1);
	RETURN(result);
}

static int vvp_io_fault_lock(const struct lu_env *env,
                             const struct cl_io_slice *ios)
{
        struct cl_io *io   = ios->cis_io;
        struct vvp_io *vio = cl2vvp_io(env, ios);
	struct cl_object*	obj = io->ci_obj;
	struct inode		*inode = vvp_object_inode(obj);
	struct ll_inode_info	*lli = ll_i2info(inode);

	down_read(&lli->lli_trunc_sem);
        /*
         * XXX LDLM_FL_CBPENDING
         */
	return vvp_io_one_lock_index(env,
				     io, 0,
				     vvp_mode_from_vma(vio->u.fault.ft_vma),
				     io->u.ci_fault.ft_index,
				     io->u.ci_fault.ft_index);
}

static int vvp_io_write_lock(const struct lu_env *env,
			     const struct cl_io_slice *ios)
{
	struct cl_io *io = ios->cis_io;
	loff_t start;
	loff_t end;

	if (io->u.ci_wr.wr_append) {
		start = 0;
		end   = OBD_OBJECT_EOF;
	} else {
		start = io->u.ci_wr.wr.crw_pos;
		end   = start + io->u.ci_wr.wr.crw_count - 1;
	}

	RETURN(vvp_io_rw_lock(env, io, CLM_WRITE, start, end));
}

static int vvp_io_setattr_iter_init(const struct lu_env *env,
				    const struct cl_io_slice *ios)

{
	return 0;
}

/**
 * Implementation of cl_io_operations::cio_lock() method for CIT_SETATTR io.
 *
 * Handles "lockless io" mode when extent locking is done by server.
 */
static int vvp_io_setattr_lock(const struct lu_env *env,
                               const struct cl_io_slice *ios)
{
	struct cl_io  *io  = ios->cis_io;
	struct inode		*inode = vvp_object_inode(io->ci_obj);
	struct ll_inode_info	*lli   = ll_i2info(inode);
	__u64 new_size;
	__u32 enqflags = 0;

	if (cl_io_is_trunc(io)) {
		down_write(&lli->lli_trunc_sem);

		new_size = io->u.ci_setattr.sa_attr.lvb_size;
		if (new_size == 0)
			enqflags = CEF_DISCARD_DATA;
	} else {
		unsigned int valid = io->u.ci_setattr.sa_avalid;

		if (!(valid & TIMES_SET_FLAGS))
			return 0;

		if ((!(valid & ATTR_MTIME) ||
		     io->u.ci_setattr.sa_attr.lvb_mtime >=
		     io->u.ci_setattr.sa_attr.lvb_ctime) &&
		    (!(valid & ATTR_ATIME) ||
		     io->u.ci_setattr.sa_attr.lvb_atime >=
		     io->u.ci_setattr.sa_attr.lvb_ctime))
			return 0;

		new_size = 0;
	}

	return vvp_io_one_lock(env, io, enqflags, CLM_WRITE,
			       new_size, OBD_OBJECT_EOF);
}

static void vvp_io_setattr_unlock(const struct lu_env *env,
                               const struct cl_io_slice *ios)
{
	struct cl_io		*io    = ios->cis_io;
	struct inode		*inode = vvp_object_inode(io->ci_obj);
	struct ll_inode_info	*lli   = ll_i2info(inode);

	if (cl_io_is_trunc(io))
		up_write(&lli->lli_trunc_sem);
}

static int vvp_do_vmtruncate(struct inode *inode, size_t size)
{
	int     result;

	/*
	 * Only ll_inode_size_lock is taken at this level.
	 */
	ll_inode_size_lock(inode);
	result = inode_newsize_ok(inode, size);
	if (result < 0) {
		ll_inode_size_unlock(inode);
		return result;
	}
	i_size_write(inode, size);

	ll_truncate_pagecache(inode, size);
	ll_inode_size_unlock(inode);
	return result;
}

static int vvp_io_setattr_time(const struct lu_env *env,
                               const struct cl_io_slice *ios)
{
        struct cl_io       *io    = ios->cis_io;
        struct cl_object   *obj   = io->ci_obj;
	struct cl_attr     *attr  = vvp_env_thread_attr(env);
        int result;
        unsigned valid = CAT_CTIME;

	cl_object_attr_lock(obj);
	attr->cat_ctime = io->u.ci_setattr.sa_attr.lvb_ctime;
	if (io->u.ci_setattr.sa_avalid & ATTR_ATIME_SET) {
		attr->cat_atime = io->u.ci_setattr.sa_attr.lvb_atime;
		valid |= CAT_ATIME;
	}
	if (io->u.ci_setattr.sa_avalid & ATTR_MTIME_SET) {
		attr->cat_mtime = io->u.ci_setattr.sa_attr.lvb_mtime;
		valid |= CAT_MTIME;
	}
	result = cl_object_attr_update(env, obj, attr, valid);
	cl_object_attr_unlock(obj);

	return result;
}

static int vvp_io_setattr_start(const struct lu_env *env,
				const struct cl_io_slice *ios)
{
	struct cl_io		*io    = ios->cis_io;
	struct inode		*inode = vvp_object_inode(io->ci_obj);
	struct ll_inode_info	*lli   = ll_i2info(inode);

	OBD_FAIL_TIMEOUT(OBD_FAIL_LLITE_TRUNCATE_INODE_PAUSE, cfs_fail_val);

	mutex_lock(&lli->lli_setattr_mutex);

	if (cl_io_is_trunc(io))
		inode_dio_wait(inode);

	if (io->u.ci_setattr.sa_avalid & TIMES_SET_FLAGS)
		return vvp_io_setattr_time(env, ios);

	return 0;
}

static void vvp_io_setattr_end(const struct lu_env *env,
                               const struct cl_io_slice *ios)
{
	struct cl_io		*io    = ios->cis_io;
	struct inode		*inode = vvp_object_inode(io->ci_obj);
	struct ll_inode_info	*lli   = ll_i2info(inode);

	if (cl_io_is_trunc(io)) {
		/* Truncate in memory pages - they must be clean pages
		 * because osc has already notified to destroy osc_extents. */
		vvp_do_vmtruncate(inode, io->u.ci_setattr.sa_attr.lvb_size);
		inode_dio_write_done(inode);
	}
	mutex_unlock(&lli->lli_setattr_mutex);
}

static void vvp_io_setattr_fini(const struct lu_env *env,
				const struct cl_io_slice *ios)
{
	bool restore_needed = ios->cis_io->ci_restore_needed;
	struct inode *inode = vvp_object_inode(ios->cis_obj);

	vvp_io_fini(env, ios);

	if (restore_needed && !ios->cis_io->ci_restore_needed) {
		/* restore finished, set data modified flag for HSM */
		ll_file_set_flag(ll_i2info(inode), LLIF_DATA_MODIFIED);
	}
}

static int vvp_io_read_start(const struct lu_env *env,
			     const struct cl_io_slice *ios)
{
	struct vvp_io		*vio   = cl2vvp_io(env, ios);
	struct cl_io		*io    = ios->cis_io;
	struct cl_object	*obj   = io->ci_obj;
	struct inode		*inode = vvp_object_inode(obj);
	struct file		*file  = vio->vui_fd->fd_file;
	loff_t  pos = io->u.ci_rd.rd.crw_pos;
	long    cnt = io->u.ci_rd.rd.crw_count;
	long    tot = vio->vui_tot_count;
	int     exceed = 0;
	int     result;
	ENTRY;

	CLOBINVRNT(env, obj, vvp_object_invariant(obj));

	CDEBUG(D_VFSTRACE, "%s: read [%llu, %llu)\n",
		file_dentry(file)->d_name.name,
		pos, pos + cnt);

	if (!can_populate_pages(env, io, inode))
		RETURN(0);

	/* Unless this is reading a sparse file, otherwise the lock has already
	 * been acquired so vvp_prep_size() is an empty op. */
	result = vvp_prep_size(env, obj, io, pos, cnt, &exceed);
	if (result != 0)
		RETURN(result);
	else if (exceed != 0)
		GOTO(out, result);

	LU_OBJECT_HEADER(D_INODE, env, &obj->co_lu,
			 "Read ino %lu, %lu bytes, offset %lld, size %llu\n",
			 inode->i_ino, cnt, pos, i_size_read(inode));

	/* turn off the kernel's read-ahead */
	vio->vui_fd->fd_file->f_ra.ra_pages = 0;

	/* initialize read-ahead window once per syscall */
	if (!vio->vui_ra_valid) {
		vio->vui_ra_valid = true;
		vio->vui_ra_start = cl_index(obj, pos);
		vio->vui_ra_count = cl_index(obj, tot + PAGE_SIZE - 1);
	}

	/* BUG: 5972 */
	file_accessed(file);
	switch (vio->vui_io_subtype) {
	case IO_NORMAL:
		LASSERT(vio->vui_iocb->ki_pos == pos);
		result = generic_file_read_iter(vio->vui_iocb, vio->vui_iter);
		break;
	case IO_SPLICE:
		result = generic_file_splice_read(file, &pos,
						  vio->u.splice.vui_pipe, cnt,
						  vio->u.splice.vui_flags);
		/* LU-1109: do splice read stripe by stripe otherwise if it
		 * may make nfsd stuck if this read occupied all internal pipe
		 * buffers. */
		io->ci_continue = 0;
		break;
	default:
		CERROR("Wrong IO type %u\n", vio->vui_io_subtype);
		LBUG();
	}
	GOTO(out, result);

out:
	if (result >= 0) {
		if (result < cnt)
			io->ci_continue = 0;
		io->ci_nob += result;
		result = 0;
	}

	return result;
}

static int vvp_io_commit_sync(const struct lu_env *env, struct cl_io *io,
			      struct cl_page_list *plist, int from, int to)
{
	struct cl_2queue *queue = &io->ci_queue;
	struct cl_page *page;
	unsigned int bytes = 0;
	int rc = 0;
	ENTRY;

	if (plist->pl_nr == 0)
		RETURN(0);

	if (from > 0 || to != PAGE_SIZE) {
		page = cl_page_list_first(plist);
		if (plist->pl_nr == 1) {
			cl_page_clip(env, page, from, to);
		} else {
			if (from > 0)
				cl_page_clip(env, page, from, PAGE_SIZE);
			if (to != PAGE_SIZE) {
				page = cl_page_list_last(plist);
				cl_page_clip(env, page, 0, to);
			}
		}
	}

	cl_2queue_init(queue);
	cl_page_list_splice(plist, &queue->c2_qin);
	rc = cl_io_submit_sync(env, io, CRT_WRITE, queue, 0);

	/* plist is not sorted any more */
	cl_page_list_splice(&queue->c2_qin, plist);
	cl_page_list_splice(&queue->c2_qout, plist);
	cl_2queue_fini(env, queue);

	if (rc == 0) {
		/* calculate bytes */
		bytes = plist->pl_nr << PAGE_SHIFT;
		bytes -= from + PAGE_SIZE - to;

		while (plist->pl_nr > 0) {
			page = cl_page_list_first(plist);
			cl_page_list_del(env, plist, page);

			cl_page_clip(env, page, 0, PAGE_SIZE);

			SetPageUptodate(cl_page_vmpage(page));
			cl_page_disown(env, io, page);

			/* held in ll_cl_init() */
			lu_ref_del(&page->cp_reference, "cl_io", io);
			cl_page_put(env, page);
		}
	}

	RETURN(bytes > 0 ? bytes : rc);
}

/* Taken from kernel set_page_dirty, __set_page_dirty_nobuffers
 * Last change to this area: b93b016313b3ba8003c3b8bb71f569af91f19fc7
 *
 * Current with Linus tip of tree (7/13/2019):
 * v5.2-rc4-224-ge01e060fe0
 *
 * Backwards compat for 3.x, 4.x kernels relating to memcg handling
 * & rename of radix tree to xarray. */
void vvp_set_pagevec_dirty(struct pagevec *pvec)
{
	struct page *page = pvec->pages[0];
	struct address_space *mapping = page->mapping;
#if defined HAVE_ACCOUNT_PAGE_DIRTIED_3ARGS
	struct mem_cgroup *memcg;
#endif
	unsigned long flags;
	int count = pagevec_count(pvec);
	int dirtied = 0;
	int i = 0;

	ENTRY;

	/* From set_page_dirty */
	for (i = 0; i < count; i++)
		ClearPageReclaim(pvec->pages[i]);

	LASSERTF(page->mapping,
		 "mapping must be set. page %p, page->private (cl_page) %p",
		 page, (void *) page->private);

	/* Rest of code derived from __set_page_dirty_nobuffers */
	xa_lock_irqsave(&mapping->i_pages, flags);

	/* Notes on differences with __set_page_dirty_nobuffers:
	 * 1. We don't need to call page_mapping because we know this is a page
	 * cache page.
	 * 2. We have the pages locked, so there is no need for the careful
	 * mapping/mapping2 dance.
	 * 3. No mapping is impossible. (Race w/truncate mentioned in
	 * dirty_nobuffers should be impossible because we hold the page lock.)
	 * 4. All mappings are the same because i/o is only to one file.
	 * 5. We invert the lock order on lock_page_memcg(page) and the mapping
	 * xa_lock, but this is the only function that should use that pair of
	 * locks and it can't race because Lustre locks pages throughout i/o.
	 */
	for (i = 0; i < count; i++) {
		page = pvec->pages[i];
		lock_page_memcg(page);
		if (TestSetPageDirty(page)) {
			unlock_page_memcg(page);
			continue;
		}
		LASSERTF(page->mapping == mapping,
			 "all pages must have the same mapping.  page %p, mapping %p, first mapping %p\n",
			 page, page->mapping, mapping);
		WARN_ON_ONCE(!PagePrivate(page) && !PageUptodate(page));
#ifdef HAVE_ACCOUNT_PAGE_DIRTIED_3ARGS
		memcg = mem_cgroup_begin_page_stat(page);
		account_page_dirtied(page, mapping, memcg);
		mem_cgroup_end_page_stat(memcg);
#else
		account_page_dirtied(page, mapping);
#endif
		radix_tree_tag_set(&mapping->page_tree, page_index(page),
				   PAGECACHE_TAG_DIRTY);
		dirtied++;
		unlock_page_memcg(page);
	}
	xa_unlock_irqrestore(&mapping->i_pages, flags);

	CDEBUG(D_VFSTRACE, "mapping %p, count %d, dirtied %d\n", mapping,
	       count, dirtied);

	if (mapping->host && dirtied) {
		/* !PageAnon && !swapper_space */
		__mark_inode_dirty(mapping->host, I_DIRTY_PAGES);
	}

	EXIT;
}

static void write_commit_callback(const struct lu_env *env, struct cl_io *io,
				  struct pagevec *pvec)
{
	int count = 0;
	int i = 0;

	ENTRY;

	count = pagevec_count(pvec);
	LASSERT(count > 0);

	for (i = 0; i < count; i++) {
		struct page *vmpage = pvec->pages[i];
		SetPageUptodate(vmpage);
	}

	vvp_set_pagevec_dirty(pvec);

	for (i = 0; i < count; i++) {
		struct page *vmpage = pvec->pages[i];
		struct cl_page *page = (struct cl_page *) vmpage->private;
		cl_page_disown(env, io, page);
		lu_ref_del(&page->cp_reference, "cl_io", cl_io_top(io));
		cl_page_put(env, page);
	}

	EXIT;
}

/* make sure the page list is contiguous */
static bool page_list_sanity_check(struct cl_object *obj,
				   struct cl_page_list *plist)
{
	struct cl_page *page;
	pgoff_t index = CL_PAGE_EOF;

	cl_page_list_for_each(page, plist) {
		struct vvp_page *vpg = cl_object_page_slice(obj, page);

		if (index == CL_PAGE_EOF) {
			index = vvp_index(vpg);
			continue;
		}

		++index;
		if (index == vvp_index(vpg))
			continue;

		return false;
	}
	return true;
}

/* Return how many bytes have queued or written */
int vvp_io_write_commit(const struct lu_env *env, struct cl_io *io)
{
	struct cl_object *obj = io->ci_obj;
	struct inode *inode = vvp_object_inode(obj);
	struct vvp_io *vio = vvp_env_io(env);
	struct cl_page_list *queue = &vio->u.write.vui_queue;
	struct cl_page *page;
	int rc = 0;
	int bytes = 0;
	unsigned int npages = vio->u.write.vui_queue.pl_nr;
	ENTRY;

	if (npages == 0)
		RETURN(0);

	CDEBUG(D_VFSTRACE, "commit async pages: %d, from %d, to %d\n",
		npages, vio->u.write.vui_from, vio->u.write.vui_to);

	LASSERT(page_list_sanity_check(obj, queue));

	/* submit IO with async write */
	rc = cl_io_commit_async(env, io, queue,
				vio->u.write.vui_from, vio->u.write.vui_to,
				write_commit_callback);
	npages -= queue->pl_nr; /* already committed pages */
	if (npages > 0) {
		/* calculate how many bytes were written */
		bytes = npages << PAGE_SHIFT;

		/* first page */
		bytes -= vio->u.write.vui_from;
		if (queue->pl_nr == 0) /* last page */
			bytes -= PAGE_SIZE - vio->u.write.vui_to;
		LASSERTF(bytes > 0, "bytes = %d, pages = %d\n", bytes, npages);

		vio->u.write.vui_written += bytes;

		CDEBUG(D_VFSTRACE, "Committed %d pages %d bytes, tot: %ld\n",
			npages, bytes, vio->u.write.vui_written);

		/* the first page must have been written. */
		vio->u.write.vui_from = 0;
	}
	LASSERT(page_list_sanity_check(obj, queue));
	LASSERT(ergo(rc == 0, queue->pl_nr == 0));

	/* out of quota, try sync write */
	if (rc == -EDQUOT && !cl_io_is_mkwrite(io)) {
		rc = vvp_io_commit_sync(env, io, queue,
					vio->u.write.vui_from,
					vio->u.write.vui_to);
		if (rc > 0) {
			vio->u.write.vui_written += rc;
			rc = 0;
		}
	}

	/* update inode size */
	ll_merge_attr(env, inode);

	/* Now the pages in queue were failed to commit, discard them
	 * unless they were dirtied before. */
	while (queue->pl_nr > 0) {
		page = cl_page_list_first(queue);
		cl_page_list_del(env, queue, page);

		if (!PageDirty(cl_page_vmpage(page)))
			cl_page_discard(env, io, page);

		cl_page_disown(env, io, page);

		/* held in ll_cl_init() */
		lu_ref_del(&page->cp_reference, "cl_io", io);
		cl_page_put(env, page);
	}
	cl_page_list_fini(env, queue);

	RETURN(rc);
}

static int vvp_io_write_start(const struct lu_env *env,
                              const struct cl_io_slice *ios)
{
	struct vvp_io		*vio   = cl2vvp_io(env, ios);
	struct cl_io		*io    = ios->cis_io;
	struct cl_object	*obj   = io->ci_obj;
	struct inode		*inode = vvp_object_inode(obj);
	struct file		*file  = vio->vui_fd->fd_file;
	ssize_t			 result = 0;
	loff_t			 pos = io->u.ci_wr.wr.crw_pos;
	size_t			 cnt = io->u.ci_wr.wr.crw_count;
	size_t nob = io->ci_nob;
	size_t written = 0;

	ENTRY;

	if (!can_populate_pages(env, io, inode))
		RETURN(0);

	if (cl_io_is_append(io)) {
		/*
		 * PARALLEL IO This has to be changed for parallel IO doing
		 * out-of-order writes.
		 */
		ll_merge_attr(env, inode);
		pos = io->u.ci_wr.wr.crw_pos = i_size_read(inode);
		vio->vui_iocb->ki_pos = pos;
	} else {
		LASSERTF(vio->vui_iocb->ki_pos == pos,
			 "ki_pos %lld [%lld, %lld)\n",
			 vio->vui_iocb->ki_pos,
			 pos, pos + cnt);
	}

	CDEBUG(D_VFSTRACE, "%s: write [%llu, %llu)\n",
		file_dentry(file)->d_name.name,
		pos, pos + cnt);

	/* The maximum Lustre file size is variable, based on the OST maximum
	 * object size and number of stripes.  This needs another check in
	 * addition to the VFS checks earlier. */
	if (pos + cnt > ll_file_maxbytes(inode)) {
		CDEBUG(D_INODE,
		       "%s: file %s ("DFID") offset %llu > maxbytes %llu\n",
		       ll_i2sbi(inode)->ll_fsname,
		       file_dentry(file)->d_name.name,
		       PFID(ll_inode2fid(inode)), pos + cnt,
		       ll_file_maxbytes(inode));
		RETURN(-EFBIG);
	}

	if (vio->vui_iter == NULL) {
		/* from a temp io in ll_cl_init(). */
		result = 0;
	} else {
		/*
		 * this is to turn file_remove_privs() into no-op to
		 * eliminate sending enqueue rpc from
		 * generic_getxattr() because here mdc/osc ldlm lock
		 * is held in use
		 */
		set_IS_NOSEC(inode);
		result = __generic_file_write_iter(vio->vui_iocb,
						   vio->vui_iter);
		restore_IS_NOSEC(inode);

		written = result;
		if (result > 0 || result == -EIOCBQUEUED)
#ifdef HAVE_GENERIC_WRITE_SYNC_2ARGS
			result = generic_write_sync(vio->vui_iocb, result);
#else
		{
			ssize_t err;

			err = generic_write_sync(vio->vui_iocb->ki_filp, pos,
						 result);
			if (err < 0 && result > 0)
				result = err;
		}
#endif
	}

	if (result > 0) {
		result = vvp_io_write_commit(env, io);
		if (vio->u.write.vui_written > 0) {
			result = vio->u.write.vui_written;
			CDEBUG(D_VFSTRACE, "%s: write nob %zd, result: %zd\n",
				file_dentry(file)->d_name.name,
				io->ci_nob, result);
			io->ci_nob += result;
		}
	}
	if (vio->vui_iocb->ki_pos != (pos + io->ci_nob - nob)) {
		CDEBUG(D_VFSTRACE, "%s: write position mismatch: "
		       "ki_pos %lld vs. pos %lld, written %ld, commit %ld "
		       "rc %ld\n",
		       file_dentry(file)->d_name.name,
		       vio->vui_iocb->ki_pos, pos + io->ci_nob - nob,
		       written, io->ci_nob - nob, result);
		/* rewind ki_pos to where it has successfully committed */
		vio->vui_iocb->ki_pos = pos + io->ci_nob - nob;
	}
	if (result > 0) {
		ll_file_set_flag(ll_i2info(inode), LLIF_DATA_MODIFIED);

		if (result < cnt)
			io->ci_continue = 0;
		result = 0;
	}

	RETURN(result);
}

static void vvp_io_rw_unlock(const struct lu_env *env,
			  const struct cl_io_slice *ios)
{
	struct vvp_io		*vio = cl2vvp_io(env, ios);
	struct inode		*inode = vvp_object_inode(ios->cis_obj);
	struct ll_inode_info	*lli = ll_i2info(inode);

	if (vio->vui_io_subtype == IO_NORMAL)
		up_read(&lli->lli_trunc_sem);
}

static int vvp_io_kernel_fault(struct vvp_fault_io *cfio)
{
	struct vm_fault *vmf = cfio->ft_vmf;

	cfio->ft_flags = ll_filemap_fault(cfio->ft_vma, vmf);
	cfio->ft_flags_valid = 1;

	if (vmf->page) {
		LL_CDEBUG_PAGE(D_PAGE, vmf->page, "got addr %p type NOPAGE\n",
			       get_vmf_address(vmf));
		if (unlikely(!(cfio->ft_flags & VM_FAULT_LOCKED))) {
			lock_page(vmf->page);
			cfio->ft_flags |= VM_FAULT_LOCKED;
		}

		cfio->ft_vmpage = vmf->page;

		return 0;
	}

	if (cfio->ft_flags & VM_FAULT_SIGBUS) {
		CDEBUG(D_PAGE, "got addr %p - SIGBUS\n", get_vmf_address(vmf));
		return -EFAULT;
	}

	if (cfio->ft_flags & VM_FAULT_OOM) {
		CDEBUG(D_PAGE, "got addr %p - OOM\n", get_vmf_address(vmf));
		return -ENOMEM;
	}

	if (cfio->ft_flags & VM_FAULT_RETRY)
		return -EAGAIN;

	CERROR("unknown error in page fault %d\n", cfio->ft_flags);

	return -EINVAL;
}

static void mkwrite_commit_callback(const struct lu_env *env, struct cl_io *io,
				    struct pagevec *pvec)
{
	vvp_set_pagevec_dirty(pvec);
}

static int vvp_io_fault_start(const struct lu_env *env,
                              const struct cl_io_slice *ios)
{
	struct vvp_io		*vio   = cl2vvp_io(env, ios);
	struct cl_io		*io    = ios->cis_io;
	struct cl_object	*obj   = io->ci_obj;
	struct inode		*inode = vvp_object_inode(obj);
	struct cl_fault_io	*fio   = &io->u.ci_fault;
	struct vvp_fault_io	*cfio  = &vio->u.fault;
	loff_t			 offset;
	int			 result = 0;
	struct page		*vmpage = NULL;
	struct cl_page		*page;
	loff_t			 size;
	pgoff_t			 last_index;
	ENTRY;

        /* offset of the last byte on the page */
        offset = cl_offset(obj, fio->ft_index + 1) - 1;
        LASSERT(cl_index(obj, offset) == fio->ft_index);
	result = vvp_prep_size(env, obj, io, 0, offset + 1, NULL);
	if (result != 0)
		RETURN(result);

	/* must return locked page */
	if (fio->ft_mkwrite) {
		LASSERT(cfio->ft_vmpage != NULL);
		lock_page(cfio->ft_vmpage);
	} else {
		result = vvp_io_kernel_fault(cfio);
		if (result != 0)
			RETURN(result);
	}

	vmpage = cfio->ft_vmpage;
	LASSERT(PageLocked(vmpage));

	if (OBD_FAIL_CHECK(OBD_FAIL_LLITE_FAULT_TRUNC_RACE))
		ll_invalidate_page(vmpage);

	size = i_size_read(inode);
        /* Though we have already held a cl_lock upon this page, but
         * it still can be truncated locally. */
	if (unlikely((vmpage->mapping != inode->i_mapping) ||
		     (page_offset(vmpage) > size))) {
                CDEBUG(D_PAGE, "llite: fault and truncate race happened!\n");

                /* return +1 to stop cl_io_loop() and ll_fault() will catch
                 * and retry. */
                GOTO(out, result = +1);
        }

	last_index = cl_index(obj, size - 1);

	if (fio->ft_mkwrite ) {
		/*
		 * Capture the size while holding the lli_trunc_sem from above
		 * we want to make sure that we complete the mkwrite action
		 * while holding this lock. We need to make sure that we are
		 * not past the end of the file.
		 */
		if (last_index < fio->ft_index) {
			CDEBUG(D_PAGE,
				"llite: mkwrite and truncate race happened: "
				"%p: 0x%lx 0x%lx\n",
				vmpage->mapping,fio->ft_index,last_index);
			/*
			 * We need to return if we are
			 * passed the end of the file. This will propagate
			 * up the call stack to ll_page_mkwrite where
			 * we will return VM_FAULT_NOPAGE. Any non-negative
			 * value returned here will be silently
			 * converted to 0. If the vmpage->mapping is null
			 * the error code would be converted back to ENODATA
			 * in ll_page_mkwrite0. Thus we return -ENODATA
			 * to handle both cases
			 */
			GOTO(out, result = -ENODATA);
		}
	}

	page = cl_page_find(env, obj, fio->ft_index, vmpage, CPT_CACHEABLE);
	if (IS_ERR(page))
		GOTO(out, result = PTR_ERR(page));

	/* if page is going to be written, we should add this page into cache
	 * earlier. */
	if (fio->ft_mkwrite) {
		wait_on_page_writeback(vmpage);
		if (!PageDirty(vmpage)) {
			struct cl_page_list *plist = &vio->u.fault.ft_queue;
			struct vvp_page *vpg = cl_object_page_slice(obj, page);
			int to = PAGE_SIZE;

			/* vvp_page_assume() calls wait_on_page_writeback(). */
			cl_page_assume(env, io, page);

			cl_page_list_init(plist);
			cl_page_list_add(plist, page);

			/* size fixup */
			if (last_index == vvp_index(vpg))
				to = ((size - 1) & ~PAGE_MASK) + 1;

			/* Do not set Dirty bit here so that in case IO is
			 * started before the page is really made dirty, we
			 * still have chance to detect it. */
			result = cl_io_commit_async(env, io, plist, 0, to,
						    mkwrite_commit_callback);
			/* Have overquota flag, trying sync write to check whether
 			 * indeed out of quota */
			if (result == -EDQUOT) {
				cl_page_get(page);
				result = vvp_io_commit_sync(env, io, plist, 0, to);
				if (result >= 0) {
					io->ci_noquota = 1;
					cl_page_own(env, io, page);
					cl_page_list_add(plist, page);
					lu_ref_add(&page->cp_reference, "cl_io", io);
					result = cl_io_commit_async(env, io, plist, 0, to,
							mkwrite_commit_callback);
					io->ci_noquota = 0;
				} else {
					cl_page_put(env, page);
				}
			}

			LASSERT(cl_page_is_owned(page, io));
			cl_page_list_fini(env, plist);

			vmpage = NULL;
			if (result < 0) {
				cl_page_discard(env, io, page);
				cl_page_disown(env, io, page);

				cl_page_put(env, page);

				/* we're in big trouble, what can we do now? */
				if (result == -EDQUOT)
					result = -ENOSPC;
				GOTO(out, result);
			} else {
				cl_page_disown(env, io, page);
			}
		}
	}

	/*
	 * The ft_index is only used in the case of
	 * a mkwrite action. We need to check
	 * our assertions are correct, since
	 * we should have caught this above
	 */
	LASSERT(!fio->ft_mkwrite || fio->ft_index <= last_index);
	if (fio->ft_index == last_index)
                /*
                 * Last page is mapped partially.
                 */
                fio->ft_nob = size - cl_offset(obj, fio->ft_index);
        else
                fio->ft_nob = cl_page_size(obj);

        lu_ref_add(&page->cp_reference, "fault", io);
        fio->ft_page = page;
        EXIT;

out:
	/* return unlocked vmpage to avoid deadlocking */
	if (vmpage != NULL)
		unlock_page(vmpage);

	cfio->ft_flags &= ~VM_FAULT_LOCKED;

	return result;
}

static void vvp_io_fault_unlock(const struct lu_env *env,
			     const struct cl_io_slice *ios)
{
	struct inode		*inode = vvp_object_inode(ios->cis_obj);
	struct ll_inode_info	*lli   = ll_i2info(inode);

	CLOBINVRNT(env, ios->cis_io->ci_obj,
		   vvp_object_invariant(ios->cis_io->ci_obj));
	up_read(&lli->lli_trunc_sem);
}

static int vvp_io_fsync_start(const struct lu_env *env,
			      const struct cl_io_slice *ios)
{
	/* we should mark TOWRITE bit to each dirty page in radix tree to
	 * verify pages have been written, but this is difficult because of
	 * race. */
	return 0;
}

static int vvp_io_read_ahead(const struct lu_env *env,
			     const struct cl_io_slice *ios,
			     pgoff_t start, struct cl_read_ahead *ra)
{
	int result = 0;
	ENTRY;

	if (ios->cis_io->ci_type == CIT_READ ||
	    ios->cis_io->ci_type == CIT_FAULT) {
		struct vvp_io *vio = cl2vvp_io(env, ios);

		if (unlikely(vio->vui_fd->fd_flags & LL_FILE_GROUP_LOCKED)) {
			ra->cra_end = CL_PAGE_EOF;
			result = +1; /* no need to call down */
		}
	}

	RETURN(result);
}

static const struct cl_io_operations vvp_io_ops = {
	.op = {
		[CIT_READ] = {
			.cio_fini	= vvp_io_fini,
			.cio_lock	= vvp_io_read_lock,
			.cio_unlock	= vvp_io_rw_unlock,
			.cio_start	= vvp_io_read_start,
			.cio_advance	= vvp_io_advance,
		},
                [CIT_WRITE] = {
			.cio_fini      = vvp_io_fini,
			.cio_iter_init = vvp_io_write_iter_init,
			.cio_iter_fini = vvp_io_write_iter_fini,
			.cio_lock      = vvp_io_write_lock,
			.cio_unlock    = vvp_io_rw_unlock,
			.cio_start     = vvp_io_write_start,
			.cio_advance   = vvp_io_advance,
                },
                [CIT_SETATTR] = {
                        .cio_fini       = vvp_io_setattr_fini,
                        .cio_iter_init  = vvp_io_setattr_iter_init,
                        .cio_lock       = vvp_io_setattr_lock,
			.cio_unlock	= vvp_io_setattr_unlock,
                        .cio_start      = vvp_io_setattr_start,
                        .cio_end        = vvp_io_setattr_end
                },
                [CIT_FAULT] = {
                        .cio_fini      = vvp_io_fault_fini,
                        .cio_iter_init = vvp_io_fault_iter_init,
                        .cio_lock      = vvp_io_fault_lock,
			.cio_unlock    = vvp_io_fault_unlock,
                        .cio_start     = vvp_io_fault_start,
                },
		[CIT_FSYNC] = {
			.cio_start	= vvp_io_fsync_start,
			.cio_fini	= vvp_io_fini
		},
		[CIT_GLIMPSE] = {
			.cio_fini	= vvp_io_fini
		},
		[CIT_MISC] = {
			.cio_fini	= vvp_io_fini
		},
		[CIT_LADVISE] = {
			.cio_fini	= vvp_io_fini
		},
	},
	.cio_read_ahead = vvp_io_read_ahead
};

int vvp_io_init(const struct lu_env *env, struct cl_object *obj,
                struct cl_io *io)
{
	struct vvp_io      *vio   = vvp_env_io(env);
	struct inode       *inode = vvp_object_inode(obj);
	int                 result;

	CLOBINVRNT(env, obj, vvp_object_invariant(obj));
	ENTRY;

	CDEBUG(D_VFSTRACE, DFID" ignore/verify layout %d/%d, layout version %d "
	       "restore needed %d\n",
	       PFID(lu_object_fid(&obj->co_lu)),
	       io->ci_ignore_layout, io->ci_verify_layout,
	       vio->vui_layout_gen, io->ci_restore_needed);

	CL_IO_SLICE_CLEAN(vio, vui_cl);
	cl_io_slice_add(io, &vio->vui_cl, obj, &vvp_io_ops);
	vio->vui_ra_valid = false;
	result = 0;
	if (io->ci_type == CIT_READ || io->ci_type == CIT_WRITE) {
		size_t count;
		struct ll_inode_info *lli = ll_i2info(inode);

		count = io->u.ci_rw.crw_count;
		/* "If nbyte is 0, read() will return 0 and have no other
		 *  results."  -- Single Unix Spec */
		if (count == 0)
			result = 1;
		else
			vio->vui_tot_count = count;

		/* for read/write, we store the jobid in the inode, and
		 * it'll be fetched by osc when building RPC.
		 *
		 * it's not accurate if the file is shared by different
		 * jobs.
		 */
		lustre_get_jobid(lli->lli_jobid, sizeof(lli->lli_jobid));
	} else if (io->ci_type == CIT_SETATTR) {
		if (!cl_io_is_trunc(io))
			io->ci_lockreq = CILR_MANDATORY;
	}

	/* Enqueue layout lock and get layout version. We need to do this
	 * even for operations requiring to open file, such as read and write,
	 * because it might not grant layout lock in IT_OPEN. */
	if (result == 0 && !io->ci_ignore_layout) {
		result = ll_layout_refresh(inode, &vio->vui_layout_gen);
		if (result == -ENOENT)
			/* If the inode on MDS has been removed, but the objects
			 * on OSTs haven't been destroyed (async unlink), layout
			 * fetch will return -ENOENT, we'd ingore this error
			 * and continue with dirty flush. LU-3230. */
			result = 0;
		if (result < 0)
			CERROR("%s: refresh file layout " DFID " error %d.\n",
			       ll_i2sbi(inode)->ll_fsname,
			       PFID(lu_object_fid(&obj->co_lu)), result);
	}

	io->ci_result = result < 0 ? result : 0;
	RETURN(result);
}
