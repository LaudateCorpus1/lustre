/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001  Cluster File Systems, Inc.
 *
 * This code is issued under the GNU General Public License.
 * See the file COPYING in this distribution
 */

#ifndef __OBD_H
#define __OBD_H
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/smp_lock.h>

#include <linux/lustre_idl.h>

struct obd_type {
        struct list_head typ_chain;
        struct obd_ops *typ_ops;
        char *typ_name;
        int  typ_refcnt;
};


/* Individual type definitions */

struct ext2_obd {
        struct super_block *e2_sb;
        struct vfsmount *e2_vfsmnt;
};

#define OBD_RUN_CTXT_MAGIC      0xC0FFEEAA
#define OBD_CTXT_DEBUG          /* development-only debugging */
struct obd_run_ctxt {
        struct vfsmount *pwdmnt;
        struct dentry   *pwd;
        mm_segment_t     fs;
#ifdef OBD_CTXT_DEBUG
        __u32            magic;
#endif
};

struct filter_obd {
        char *fo_fstype;
        struct super_block *fo_sb;
        struct vfsmount *fo_vfsmnt;
        struct obd_run_ctxt fo_ctxt;
        struct dentry *fo_dentry_O;
        struct dentry *fo_dentry_O_mode[16];
        spinlock_t fo_lock;
        __u64 fo_lastino;
        struct file_operations *fo_fop;
        struct inode_operations *fo_iop;
        struct address_space_operations *fo_aops;
};

struct mds_client_info;
struct mds_server_data;

struct mdc_obd {
        struct ptlrpc_client *mdc_client;
        struct ptlrpc_client *mdc_ldlm_client;
        struct ptlrpc_connection *mdc_conn;
        int mdc_max_mdsize;
        __u8 mdc_target_uuid[37];
};

struct osc_obd {
        struct ptlrpc_client *osc_client;
        struct ptlrpc_client *osc_ldlm_client;
        struct ptlrpc_connection *osc_conn;
        __u8 osc_target_uuid[37];
};

struct mds_obd {
        struct ldlm_namespace *mds_local_namespace;
        struct ptlrpc_service *mds_service;
        struct ptlrpc_client *mds_ldlm_client; /* to be an LDLM client */
        struct ptlrpc_connection *mds_ldlm_conn; /* to be an LDLM client */
        struct lustre_handle mds_connh; /* to be one's on DLM client */

        char *mds_fstype;
        struct super_block *mds_sb;
        struct super_operations *mds_sop;
        struct vfsmount *mds_vfsmnt;
        struct obd_run_ctxt mds_ctxt;
        struct file_operations *mds_fop;
        struct inode_operations *mds_iop;
        struct address_space_operations *mds_aops;
        struct mds_fs_operations *mds_fsops;
        int mds_max_mdsize;
        struct file *mds_rcvd_filp;
        __u64 mds_last_committed;
        __u64 mds_last_rcvd;
        __u64 mds_mount_count;
        struct ll_fid mds_rootfid;
        int mds_client_count;
        struct list_head mds_client_info;
        struct mds_server_data *mds_server_data;
};

struct ldlm_obd {
        struct ptlrpc_service *ldlm_service;
        struct ptlrpc_client *ldlm_client;
        struct ptlrpc_connection *ldlm_server_conn;
};

struct echo_obd {
        char *eo_fstype;
        struct super_block *eo_sb;
        struct vfsmount *eo_vfsmnt;
        struct obd_run_ctxt eo_ctxt;
        spinlock_t eo_lock;
        __u64 eo_lastino;
        struct file_operations *eo_fop;
        struct inode_operations *eo_iop;
        struct address_space_operations *eo_aops;
};

struct recovd_obd {
        time_t                recovd_waketime;
        time_t                recovd_timeout;
        struct ptlrpc_service *recovd_service;
        struct ptlrpc_client  *recovd_client;
        __u32                  recovd_flags; 
        __u32                  recovd_wakeup_flag; 
        spinlock_t             recovd_lock;
        struct list_head      recovd_clients_lh; /* clients managed  */
        struct list_head      recovd_troubled_lh; /* clients in trouble */
        wait_queue_head_t     recovd_recovery_waitq;
        wait_queue_head_t     recovd_ctl_waitq;
        wait_queue_head_t     recovd_waitq;
        struct task_struct    *recovd_thread;
};

struct trace_obd {
        struct obdtrace_opstats *stats;
};

#if 0
struct snap_obd {
        unsigned int snap_index;  /* which snapshot index are we accessing */
        int snap_tableno;
};

#endif

struct ost_obd {
        struct ptlrpc_service *ost_service;
        struct lustre_handle ost_conn;   /* the local connection to the OBD */
};


struct lov_tgt_desc { 
        uuid_t uuid;
        struct lustre_handle conn; 
};

struct lov_obd {
        struct lustre_handle mdc_connh;
        struct obd_device *mdcobd;
        struct lov_desc desc;
        int bufsize; 
        struct lov_tgt_desc *tgts;
};

struct niobuf_local {
        __u64 offset;
        __u32 len;
        __u32 xid;
        __u32 flags;
        void *addr;
        struct page *page;
        void *target_private;
        struct dentry *dentry;
};

#define N_LOCAL_TEMP_PAGE 0x00000001

/* corresponds to one of the obd's */
struct obd_device {
        struct obd_type *obd_type;

        /* common and UUID name of this device */
        char *obd_name;
        __u8 obd_uuid[37];

        int obd_minor;
        int obd_flags;
        struct proc_dir_entry *obd_proc_entry;
        struct list_head obd_exports;
        struct list_head obd_imports;
        struct ldlm_namespace *obd_namespace;
        union {
                struct ext2_obd ext2;
                struct filter_obd filter;
                struct mds_obd mds;
                struct mdc_obd mdc;
                struct ost_obd ost;
                struct osc_obd osc;
                struct ldlm_obd ldlm;
                struct echo_obd echo;
                struct recovd_obd recovd;
                struct trace_obd trace;
                struct lov_obd lov;
#if 0
                struct snap_obd snap;
#endif
        } u;
};

struct obd_ops {
        int (*o_iocontrol)(long cmd, struct lustre_handle *, int len, void *karg,
                           void *uarg);
        int (*o_get_info)(struct lustre_handle *, 
                          obd_count keylen, void *key,
                          obd_count *vallen, void **val);
        int (*o_set_info)(struct lustre_handle *, 
                          obd_count keylen, void *key,
                          obd_count vallen, void *val);
        int (*o_attach)(struct obd_device *dev, obd_count len, void *data);
        int (*o_detach)(struct obd_device *dev);
        int (*o_setup) (struct obd_device *dev, obd_count len, void *data);
        int (*o_cleanup)(struct obd_device *dev);
        int (*o_connect)(struct lustre_handle *conn, struct obd_device *src);
        int (*o_disconnect)(struct lustre_handle *conn);


        int (*o_statfs)(struct lustre_handle *conn, struct statfs *statfs);
        int (*o_preallocate)(struct lustre_handle *, obd_count *req, obd_id *ids);
        int (*o_create)(struct lustre_handle *conn,  struct obdo *oa, 
                        struct lov_stripe_md **ea);
        int (*o_destroy)(struct lustre_handle *conn, struct obdo *oa, 
                         struct lov_stripe_md *ea);
        int (*o_setattr)(struct lustre_handle *conn, struct obdo *oa);
        int (*o_getattr)(struct lustre_handle *conn, struct obdo *oa);
        int (*o_open)(struct lustre_handle *conn, struct obdo *oa,
                      struct lov_stripe_md *);
        int (*o_close)(struct lustre_handle *conn, struct obdo *oa,
                       struct lov_stripe_md *);
        int (*o_brw)(int rw, struct lustre_handle *conn, 
                     struct lov_stripe_md *md, obd_count oa_bufs, 
                     struct page **buf,
                     obd_size *count, 
                     obd_off *offset, 
                     obd_flag *flags,
                     void *);
        int (*o_punch)(struct lustre_handle *conn, struct obdo *tgt, struct lov_stripe_md *md, obd_size count,
                       obd_off offset);
        int (*o_sync)(struct lustre_handle *conn, struct obdo *tgt, obd_size count,
                      obd_off offset);
        int (*o_migrate)(struct lustre_handle *conn, struct obdo *dst,
                         struct obdo *src, obd_size count, obd_off offset);
        int (*o_copy)(struct lustre_handle *dstconn, struct obdo *dst,
                      struct lustre_handle *srconn, struct obdo *src,
                      obd_size count, obd_off offset);
        int (*o_iterate)(struct lustre_handle *conn, int (*)(obd_id, obd_gr, void *),
                         obd_id *startid, obd_gr group, void *data);
        int (*o_preprw)(int cmd, struct lustre_handle *conn,
                        int objcount, struct obd_ioobj *obj,
                        int niocount, struct niobuf_remote *remote,
                        struct niobuf_local *local, void **desc_private);
        int (*o_commitrw)(int cmd, struct lustre_handle *conn,
                          int objcount, struct obd_ioobj *obj,
                          int niocount, struct niobuf_local *local,
                          void *desc_private);
        int (*o_enqueue)(struct lustre_handle *conn,
                         struct lustre_handle *parent_lock, __u64 *res_id,
                         __u32 type, void *cookie, int cookielen, __u32 mode,
                         int *flags, void *cb, void *data, int datalen,
                         struct lustre_handle *lockh);
        int (*o_cancel)(struct lustre_handle *, __u32 mode, struct lustre_handle *);
};

#endif
