/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001, 2002 Cluster File Systems, Inc.
 *
 * This code is issued under the GNU General Public License.
 * See the file COPYING in this distribution
 */

#define DEBUG_SUBSYSTEM S_COBD

#include <linux/obd_support.h>
#include <linux/lustre_lib.h>
#include <linux/lustre_net.h>
#include <linux/lustre_idl.h>
#include <linux/obd_class.h>
#include <linux/obd_cache.h>

static int cobd_attach(struct obd_device *dev, obd_count len, void *data)
{
        struct lprocfs_static_vars lvars;

        lprocfs_init_vars(&lvars);
	return lprocfs_obd_attach(dev, lvars.obd_vars);
}

static int cobd_detach(struct obd_device *dev)
{
	return lprocfs_obd_detach(dev);
}

static int
cobd_setup (struct obd_device *dev, obd_count len, void *buf)
{
        struct obd_ioctl_data *data = (struct obd_ioctl_data *)buf;
        struct cache_obd  *cobd = &dev->u.cobd;
        struct obd_device *target;
        struct obd_device *cache;
        int                rc;

        if (data->ioc_inlbuf1 == NULL ||
            data->ioc_inlbuf2 == NULL)
                return (-EINVAL);

        target = class_uuid2obd (data->ioc_inlbuf1);
        cache  = class_uuid2obd (data->ioc_inlbuf2);
        if (target == NULL ||
            cache == NULL)
                return (-EINVAL);

        /* don't bother checking attached/setup;
         * obd_connect() should, and it can change underneath us */

        rc = obd_connect (&cobd->cobd_target, target, NULL, NULL, NULL);
        if (rc != 0)
                return (rc);

        rc = obd_connect (&cobd->cobd_cache, cache, NULL, NULL, NULL);
        if (rc != 0)
                goto fail_0;

        return (0);

 fail_0:
        obd_disconnect (&cobd->cobd_target);
        return (rc);
}

static int
cobd_cleanup (struct obd_device *dev)
{
        struct cache_obd  *cobd = &dev->u.cobd;
        int                rc;

        if (!list_empty (&dev->obd_exports))
                return (-EBUSY);

        rc = obd_disconnect (&cobd->cobd_cache);
        if (rc != 0)
                CERROR ("error %d disconnecting cache\n", rc);

        rc = obd_disconnect (&cobd->cobd_target);
        if (rc != 0)
                CERROR ("error %d disconnecting target\n", rc);

        return (0);
}

static int
cobd_connect (struct lustre_handle *conn, struct obd_device *obd,
              obd_uuid_t cluuid, struct recovd_obd *recovd,
              ptlrpc_recovery_cb_t recover)
{
        int rc = class_connect (conn, obd, cluuid);

        CERROR ("rc %d\n", rc);
        return (rc);
}

static int
cobd_disconnect (struct lustre_handle *conn)
{
	int rc = class_disconnect (conn);

        CERROR ("rc %d\n", rc);
	return (rc);
}

static int
cobd_get_info(struct lustre_handle *conn, obd_count keylen,
              void *key, obd_count *vallen, void **val)
{
        struct obd_device *obd = class_conn2obd(conn);
        struct cache_obd  *cobd;

        if (obd == NULL) {
                CERROR("invalid client "LPX64"\n", conn->addr);
                return -EINVAL;
        }

        cobd = &obd->u.cobd;

        /* intercept cache utilisation info? */

        return (obd_get_info (&cobd->cobd_target,
                              keylen, key, vallen, val));
}

static int
cobd_statfs(struct lustre_handle *conn, struct obd_statfs *osfs)
{
        struct obd_device *obd = class_conn2obd(conn);
        struct cache_obd  *cobd;

        if (obd == NULL) {
                CERROR("invalid client "LPX64"\n", conn->addr);
                return -EINVAL;
        }

        cobd = &obd->u.cobd;
        return (obd_statfs (&cobd->cobd_target, osfs));
}

static int
cobd_getattr(struct lustre_handle *conn, struct obdo *oa,
             struct lov_stripe_md *lsm)
{
        struct obd_device *obd = class_conn2obd(conn);
        struct cache_obd  *cobd;

        if (obd == NULL) {
                CERROR("invalid client "LPX64"\n", conn->addr);
                return -EINVAL;
        }

        cobd = &obd->u.cobd;
        return (obd_getattr (&cobd->cobd_target, oa, lsm));
}

static int
cobd_open(struct lustre_handle *conn, struct obdo *oa,
          struct lov_stripe_md *lsm)
{
        struct obd_device *obd = class_conn2obd(conn);
        struct cache_obd  *cobd;

        if (obd == NULL) {
                CERROR("invalid client "LPX64"\n", conn->addr);
                return -EINVAL;
        }

        cobd = &obd->u.cobd;
        return (obd_open (&cobd->cobd_target, oa, lsm));
}

static int
cobd_close(struct lustre_handle *conn, struct obdo *oa,
           struct lov_stripe_md *lsm)
{
        struct obd_device *obd = class_conn2obd(conn);
        struct cache_obd  *cobd;

        if (obd == NULL) {
                CERROR("invalid client "LPX64"\n", conn->addr);
                return -EINVAL;
        }

        cobd = &obd->u.cobd;
        return (obd_close (&cobd->cobd_target, oa, lsm));
}

static int
cobd_preprw(int cmd, struct lustre_handle *conn,
            int objcount, struct obd_ioobj *obj,
            int niocount, struct niobuf_remote *nb,
            struct niobuf_local *res, void **desc_private)
{
        struct obd_device *obd = class_conn2obd(conn);
        struct cache_obd  *cobd;

        if (obd == NULL) {
                CERROR("invalid client "LPX64"\n", conn->addr);
                return -EINVAL;
        }

        if ((cmd & OBD_BRW_WRITE) != 0)
                return -EOPNOTSUPP;

        cobd = &obd->u.cobd;
        return (obd_preprw (cmd, &cobd->cobd_target,
                            objcount, obj,
                            niocount, nb,
                            res, desc_private));
}

static int
cobd_commitrw(int cmd, struct lustre_handle *conn,
              int objcount, struct obd_ioobj *obj,
              int niocount, struct niobuf_local *local,
              void *desc_private)
{
        struct obd_device *obd = class_conn2obd(conn);
        struct cache_obd  *cobd;

        if (obd == NULL) {
                CERROR("invalid client "LPX64"\n", conn->addr);
                return -EINVAL;
        }

        if ((cmd & OBD_BRW_WRITE) != 0)
                return -EOPNOTSUPP;

        cobd = &obd->u.cobd;
        return (obd_commitrw (cmd, &cobd->cobd_target,
                              objcount, obj,
                              niocount, local,
                              desc_private));
}

static inline int
cobd_brw(int cmd, struct lustre_handle *conn,
         struct lov_stripe_md *lsm, obd_count oa_bufs,
         struct brw_page *pga, struct obd_brw_set *set)
{
        struct obd_device *obd = class_conn2obd(conn);
        struct cache_obd  *cobd;

        if (obd == NULL) {
                CERROR("invalid client "LPX64"\n", conn->addr);
                return -EINVAL;
        }

        if ((cmd & OBD_BRW_WRITE) != 0)
                return -EOPNOTSUPP;

        cobd = &obd->u.cobd;
        return (obd_brw (cmd, &cobd->cobd_target,
                         lsm, oa_bufs, pga, set));
}

static int
cobd_iocontrol(unsigned int cmd, struct lustre_handle *conn, int len,
               void *karg, void *uarg)
{
        struct obd_device *obd = class_conn2obd(conn);
        struct cache_obd  *cobd;

        if (obd == NULL) {
                CERROR("invalid client "LPX64"\n", conn->addr);
                return -EINVAL;
        }

        /* intercept? */

        cobd = &obd->u.cobd;
        return (obd_iocontrol (cmd, &cobd->cobd_target, len, karg, uarg));
}

static struct obd_ops cobd_ops = {
        o_owner:                THIS_MODULE,
        o_attach:               cobd_attach,
        o_detach:               cobd_detach,

        o_setup:                cobd_setup,
        o_cleanup:              cobd_cleanup,

        o_connect:              cobd_connect,
        o_disconnect:           cobd_disconnect,

        o_get_info:             cobd_get_info,
        o_statfs:               cobd_statfs,

        o_getattr:              cobd_getattr,
        o_open:                 cobd_open,
        o_close:                cobd_close,
        o_preprw:               cobd_preprw,
        o_commitrw:             cobd_commitrw,
        o_brw:                  cobd_brw,
        o_iocontrol:            cobd_iocontrol,
};

static int __init cobd_init(void)
{
        struct lprocfs_static_vars lvars;
        ENTRY;

	printk(KERN_INFO "Lustre Caching OBD driver\n");

        lprocfs_init_vars(&lvars);
        RETURN(class_register_type(&cobd_ops, lvars.module_vars,
                                   OBD_CACHE_DEVICENAME));
}

static void __exit cobd_exit(void)
{
	class_unregister_type(OBD_CACHE_DEVICENAME);
}

MODULE_AUTHOR("Cluster Filesystems Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Caching OBD driver");
MODULE_LICENSE("GPL");

module_init(cobd_init);
module_exit(cobd_exit);
