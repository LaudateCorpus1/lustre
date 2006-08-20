/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *   This file is part of Lustre, http://www.lustre.org
 *
 * MDS data structures.
 * See also lustre_idl.h for wire formats of requests.
 */

#ifndef _LUSTRE_ACL_H
#define _LUSTRE_ACL_H

#ifdef __KERNEL__
# include <linux/fs.h>
# include <linux/dcache.h>
# ifdef CONFIG_FS_POSIX_ACL
# include <linux/xattr_acl.h>
# endif
#endif

/* ACL */
#ifdef CONFIG_FS_POSIX_ACL
#ifdef HAVE_XATTR_ACL
#  define MDS_XATTR_NAME_ACL_ACCESS XATTR_NAME_ACL_ACCESS
#  define mds_xattr_acl_size(entry) xattr_acl_size(entry)
# else
#  define MDS_XATTR_NAME_ACL_ACCESS POSIX_ACL_XATTR_ACCESS
#  define mds_xattr_acl_size(entry) posix_acl_xattr_size(entry)
# endif
# define LUSTRE_POSIX_ACL_MAX_ENTRIES   (32)
# define LUSTRE_POSIX_ACL_MAX_SIZE      \
                (mds_xattr_acl_size(LUSTRE_POSIX_ACL_MAX_ENTRIES))
#else
# define LUSTRE_POSIX_ACL_MAX_SIZE      0
#endif

#endif
