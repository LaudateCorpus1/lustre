/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001, 2002 Cluster File Systems, Inc.
 *
 *  This code is issued under the GNU General Public License.
 *  See the file COPYING in this distribution
 *
 *  Author Peter Braam <braam@clusterfs.com>
 *
 *  This server is single threaded at present (but can easily be multi
 *  threaded). For testing and management it is treated as an
 *  obd_device, although it does not export a full OBD method table
 *  (the requests are coming in over the wire, so object target
 *  modules do not have a full method table.)
 *
 */

#define EXPORT_SYMTAB
#define DEBUG_SUBSYSTEM S_OSC

#include <linux/module.h>
#include <linux/lustre_dlm.h>
#include <linux/lustre_mds.h> /* for mds_objid */
#include <linux/obd_ost.h>
#include <linux/obd_lov.h>
#include <linux/init.h>
#include <linux/lustre_ha.h>

static int osc_getattr(struct lustre_handle *conn, struct obdo *oa, 
                       struct lov_stripe_md *md)
{
        struct ptlrpc_request *request;
        struct ost_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        request = ptlrpc_prep_req2(conn, OST_GETATTR, 1, &size, NULL);
        if (!request)
                RETURN(-ENOMEM);

        body = lustre_msg_buf(request->rq_reqmsg, 0);
#warning FIXME: pack only valid fields instead of memcpy, endianness
        memcpy(&body->oa, oa, sizeof(*oa));

        request->rq_replen = lustre_msg_size(1, &size);

        rc = ptlrpc_queue_wait(request);
        rc = ptlrpc_check_status(request, rc);
        if (rc) {
                CERROR("%s failed: rc = %d\n", __FUNCTION__, rc);
                GOTO(out, rc);
        }

        body = lustre_msg_buf(request->rq_repmsg, 0);
        CDEBUG(D_INODE, "mode: %o\n", body->oa.o_mode);
        if (oa)
                memcpy(oa, &body->oa, sizeof(*oa));

        EXIT;
 out:
        ptlrpc_free_req(request);
        return rc;
}

static int osc_open(struct lustre_handle *conn, struct obdo *oa,
                    struct lov_stripe_md *md)
{
        struct ptlrpc_request *request;
        struct ost_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        request = ptlrpc_prep_req2(conn, OST_OPEN, 1, &size, NULL);
        if (!request)
                RETURN(-ENOMEM);

        body = lustre_msg_buf(request->rq_reqmsg, 0);
#warning FIXME: pack only valid fields instead of memcpy, endianness
        memcpy(&body->oa, oa, sizeof(*oa));

        request->rq_replen = lustre_msg_size(1, &size);

        rc = ptlrpc_queue_wait(request);
        rc = ptlrpc_check_status(request, rc);
        if (rc)
                GOTO(out, rc);

        body = lustre_msg_buf(request->rq_repmsg, 0);
        CDEBUG(D_INODE, "mode: %o\n", body->oa.o_mode);
        if (oa)
                memcpy(oa, &body->oa, sizeof(*oa));

        EXIT;
 out:
        ptlrpc_free_req(request);
        return rc;
}

static int osc_close(struct lustre_handle *conn, struct obdo *oa,
                     struct lov_stripe_md *md)
{
        struct ptlrpc_request *request;
        struct ost_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        request = ptlrpc_prep_req2(conn, OST_CLOSE, 1, &size, NULL);
        if (!request)
                RETURN(-ENOMEM);

        body = lustre_msg_buf(request->rq_reqmsg, 0);
#warning FIXME: pack only valid fields instead of memcpy, endianness
        memcpy(&body->oa, oa, sizeof(*oa));

        request->rq_replen = lustre_msg_size(1, &size);

        rc = ptlrpc_queue_wait(request);
        rc = ptlrpc_check_status(request, rc);
        if (rc)
                GOTO(out, rc);

        body = lustre_msg_buf(request->rq_repmsg, 0);
        CDEBUG(D_INODE, "mode: %o\n", body->oa.o_mode);
        if (oa)
                memcpy(oa, &body->oa, sizeof(*oa));

        EXIT;
 out:
        ptlrpc_free_req(request);
        return rc;
}

static int osc_setattr(struct lustre_handle *conn, struct obdo *oa,
                       struct lov_stripe_md *md)
{
        struct ptlrpc_request *request;
        struct ost_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        request = ptlrpc_prep_req2(conn, OST_SETATTR, 1, &size, NULL);
        if (!request)
                RETURN(-ENOMEM);

        body = lustre_msg_buf(request->rq_reqmsg, 0);
        memcpy(&body->oa, oa, sizeof(*oa));

        request->rq_replen = lustre_msg_size(1, &size);

        rc = ptlrpc_queue_wait(request);
        rc = ptlrpc_check_status(request, rc);
        GOTO(out, rc);

 out:
        ptlrpc_free_req(request);
        return rc;
}

static int osc_create(struct lustre_handle *conn, struct obdo *oa,
                      struct lov_stripe_md **ea)
{
        struct ptlrpc_request *request;
        struct ost_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        if (!oa) {
                CERROR("oa NULL\n");
                RETURN(-EINVAL);
        }

        if (!ea) {
                LBUG();
        }

        if (!*ea) {
                OBD_ALLOC(*ea, oa->o_easize);
                if (!*ea)
                        RETURN(-ENOMEM);
                (*ea)->lmd_easize = oa->o_easize;
        }

        request = ptlrpc_prep_req2(conn, OST_CREATE, 1, &size, NULL);
        if (!request)
                RETURN(-ENOMEM);

        body = lustre_msg_buf(request->rq_reqmsg, 0);
        memcpy(&body->oa, oa, sizeof(*oa));

        request->rq_replen = lustre_msg_size(1, &size);

        rc = ptlrpc_queue_wait(request);
        rc = ptlrpc_check_status(request, rc);
        if (rc)
                GOTO(out, rc);

        body = lustre_msg_buf(request->rq_repmsg, 0);
        memcpy(oa, &body->oa, sizeof(*oa));

        (*ea)->lmd_object_id = oa->o_id;
        (*ea)->lmd_stripe_count = 1;
        EXIT;
 out:
        ptlrpc_free_req(request);
        return rc;
}

static int osc_punch(struct lustre_handle *conn, struct obdo *oa,
                     struct lov_stripe_md *md, obd_size start,
                     obd_size end)
{
        struct ptlrpc_request *request;
        struct ost_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        if (!oa) {
                CERROR("oa NULL\n");
                RETURN(-EINVAL);
        }

        request = ptlrpc_prep_req2(conn, OST_PUNCH, 1, &size, NULL);
        if (!request)
                RETURN(-ENOMEM);

        body = lustre_msg_buf(request->rq_reqmsg, 0);
#warning FIXME: pack only valid fields instead of memcpy, endianness, valid
        memcpy(&body->oa, oa, sizeof(*oa));

        /* overload the blocks and size fields in the oa with start/end */ 
#warning FIXME: endianness, size=start, blocks=end?
        body->oa.o_blocks = start;
        body->oa.o_size = end;
        body->oa.o_valid |= OBD_MD_FLBLOCKS | OBD_MD_FLSIZE;

        request->rq_replen = lustre_msg_size(1, &size);

        rc = ptlrpc_queue_wait(request);
        rc = ptlrpc_check_status(request, rc);
        if (rc)
                GOTO(out, rc);

        body = lustre_msg_buf(request->rq_repmsg, 0);
        memcpy(oa, &body->oa, sizeof(*oa));

        EXIT;
 out:
        ptlrpc_free_req(request);
        return rc;
}

static int osc_destroy(struct lustre_handle *conn, struct obdo *oa,
                       struct lov_stripe_md *ea)
{
        struct ptlrpc_request *request;
        struct ost_body *body;
        int rc, size = sizeof(*body);
        ENTRY;

        if (!oa) {
                CERROR("oa NULL\n");
                RETURN(-EINVAL);
        }
        request = ptlrpc_prep_req2(conn, OST_DESTROY, 1, &size, NULL);
        if (!request)
                RETURN(-ENOMEM);

        body = lustre_msg_buf(request->rq_reqmsg, 0);
#warning FIXME: pack only valid fields instead of memcpy, endianness
        memcpy(&body->oa, oa, sizeof(*oa));

        request->rq_replen = lustre_msg_size(1, &size);

        rc = ptlrpc_queue_wait(request);
        rc = ptlrpc_check_status(request, rc);
        if (rc)
                GOTO(out, rc);

        body = lustre_msg_buf(request->rq_repmsg, 0);
        memcpy(oa, &body->oa, sizeof(*oa));

        EXIT;
 out:
        ptlrpc_free_req(request);
        return rc;
}

struct osc_brw_cb_data {
        brw_callback_t callback;
        void *cb_data;
        void *obd_data;
        size_t obd_size;
};

/* Our bulk-unmapping bottom half. */
static void unmap_and_decref_bulk_desc(void *data)
{
        struct ptlrpc_bulk_desc *desc = data;
        struct list_head *tmp;
        ENTRY;

        /* This feels wrong to me. */
        list_for_each(tmp, &desc->b_page_list) {
                struct ptlrpc_bulk_page *bulk;
                bulk = list_entry(tmp, struct ptlrpc_bulk_page, b_link);

                kunmap(bulk->b_page);
        }

        ptlrpc_bulk_decref(desc);
        EXIT;
}

static void brw_finish(struct ptlrpc_bulk_desc *desc, void *data)
{
        struct osc_brw_cb_data *cb_data = data;
        int err = 0;
        ENTRY;

        if (desc->b_flags & PTL_RPC_FL_TIMEOUT) {
                err = (desc->b_flags & PTL_RPC_FL_INTR ? -ERESTARTSYS : 
                       -ETIMEDOUT);
        }

        if (cb_data->callback)
                cb_data->callback(cb_data->cb_data, err, CB_PHASE_FINISH);

        OBD_FREE(cb_data->obd_data, cb_data->obd_size);
        OBD_FREE(cb_data, sizeof(*cb_data));

        /* We can't kunmap the desc from interrupt context, so we do it from
         * the bottom half above. */
        INIT_TQUEUE(&desc->b_queue, 0, 0);
        PREPARE_TQUEUE(&desc->b_queue, unmap_and_decref_bulk_desc, desc);
        schedule_task(&desc->b_queue);

        EXIT;
}

static int osc_brw_read(struct lustre_handle *conn, struct lov_stripe_md *md,
                        obd_count page_count, struct brw_page *pga,
                        brw_callback_t callback, struct io_cb_data *data)
{
        struct ptlrpc_connection *connection = client_conn2cli(conn)->cl_conn;
        struct ptlrpc_request *request = NULL;
        struct ptlrpc_bulk_desc *desc = NULL;
        struct ost_body *body;
        struct osc_brw_cb_data *cb_data = NULL;
        int rc, size[3] = {sizeof(*body)};
        void *iooptr, *nioptr;
        int mapped = 0;
        __u32 xid;
        ENTRY;

        size[1] = sizeof(struct obd_ioobj);
        size[2] = page_count * sizeof(struct niobuf_remote);

        request = ptlrpc_prep_req2(conn, OST_READ, 3, size, NULL);
        if (!request)
                RETURN(-ENOMEM);

        body = lustre_msg_buf(request->rq_reqmsg, 0);

        desc = ptlrpc_prep_bulk(connection);
        if (!desc)
                GOTO(out_req, rc = -ENOMEM);
        desc->b_portal = OST_BULK_PORTAL;
        desc->b_cb = brw_finish;
        OBD_ALLOC(cb_data, sizeof(*cb_data));
        if (!cb_data)
                GOTO(out_desc, rc = -ENOMEM);

        cb_data->callback = callback;
        cb_data->cb_data = data;
        data->desc = desc;
        desc->b_cb_data = cb_data;

        iooptr = lustre_msg_buf(request->rq_reqmsg, 1);
        nioptr = lustre_msg_buf(request->rq_reqmsg, 2);
        ost_pack_ioo(&iooptr, md, page_count);
        /* end almost identical to brw_write case */

        spin_lock(&connection->c_lock);
        xid = ++connection->c_xid_out;       /* single xid for all pages */
        spin_unlock(&connection->c_lock);

        for (mapped = 0; mapped < page_count; mapped++) {
                struct ptlrpc_bulk_page *bulk = ptlrpc_prep_bulk_page(desc);
                if (bulk == NULL)
                        GOTO(out_unmap, rc = -ENOMEM);

                bulk->b_xid = xid;           /* single xid for all pages */

                bulk->b_buf = kmap(pga[mapped].pg);
                bulk->b_page = pga[mapped].pg;
                bulk->b_buflen = PAGE_SIZE;
                ost_pack_niobuf(&nioptr, pga[mapped].off, pga[mapped].count,
                                pga[mapped].flag, bulk->b_xid);
        }

        /*
         * Register the bulk first, because the reply could arrive out of order,
         * and we want to be ready for the bulk data.
         *
         * The reference is released when brw_finish is complete.
         *
         * On error, we never do the brw_finish, so we handle all decrefs.
         */
        rc = ptlrpc_register_bulk(desc);
        if (rc)
                GOTO(out_unmap, rc);

        request->rq_replen = lustre_msg_size(1, size);
        rc = ptlrpc_queue_wait(request);
        rc = ptlrpc_check_status(request, rc);

        /* XXX: Mike, this is the only place I'm not sure of.  If we have
         *      an error here, will we have always called brw_finish?  If no,
         *      then out_req will not clean up and we should go to out_desc.
         *      If maybe, then we are screwed, and we need to set things up
         *      so that bulk_sink_callback is called for each bulk page,
         *      even on error so brw_finish is always called.  It would need
         *      to be passed an error code as a parameter to know what to do.
         *
         *      That would also help with the partial completion case, so
         *      we could say in brw_finish "these pages are done, don't
         *      restart them" and osc_brw callers can know this.
         */
        if (rc)
                GOTO(out_req, rc);

        /* Callbacks cause asynchronous handling. */
        rc = callback(data, 0, CB_PHASE_START); 

        EXIT;
out_req:
        ptlrpc_req_finished(request);
        RETURN(rc);

        /* Clean up on error. */
out_unmap:
        while (mapped-- > 0)
                kunmap(page_array[mapped]);
        OBD_FREE(cb_data, sizeof(*cb_data));
out_desc:
        ptlrpc_bulk_decref(desc);
        goto out_req;
}

static int osc_brw_write(struct lustre_handle *conn, struct lov_stripe_md *md,
                         obd_count page_count, struct brw_page *pga,
                         brw_callback_t callback, struct io_cb_data *data)
{
        struct ptlrpc_connection *connection = client_conn2cli(conn)->cl_conn;
        struct ptlrpc_request *request = NULL;
        struct ptlrpc_bulk_desc *desc = NULL;
        struct ost_body *body;
        struct niobuf_local *local = NULL;
        struct niobuf_remote *remote;
        struct osc_brw_cb_data *cb_data = NULL;
        int rc, j, size[3] = {sizeof(*body)};
        void *iooptr, *nioptr;
        int mapped = 0;
        ENTRY;

        size[1] = sizeof(struct obd_ioobj);
        size[2] = page_count * sizeof(*remote);

        request = ptlrpc_prep_req2(conn, OST_WRITE, 3, size, NULL);
        if (!request)
                RETURN(-ENOMEM);

        body = lustre_msg_buf(request->rq_reqmsg, 0);

        desc = ptlrpc_prep_bulk(connection);
        if (!desc)
                GOTO(out_req, rc = -ENOMEM);
        desc->b_portal = OSC_BULK_PORTAL;
        desc->b_cb = brw_finish;
        OBD_ALLOC(cb_data, sizeof(*cb_data));
        if (!cb_data)
                GOTO(out_desc, rc = -ENOMEM);

        cb_data->callback = callback;
        cb_data->cb_data = data;
        data->desc = desc;
        desc->b_cb_data = cb_data;

        iooptr = lustre_msg_buf(request->rq_reqmsg, 1);
        nioptr = lustre_msg_buf(request->rq_reqmsg, 2);
        ost_pack_ioo(&iooptr, md, page_count);
        /* end almost identical to brw_read case */

        OBD_ALLOC(local, page_count * sizeof(*local));
        if (!local)
                GOTO(out_cb, rc = -ENOMEM);

        cb_data->obd_data = local;
        cb_data->obd_size = page_count * sizeof(*local);

        for (mapped = 0; mapped < page_count; mapped++) {
                local[mapped].addr = kmap(pga[mapped].pg);
                local[mapped].offset = pga[mapped].off;
                local[mapped].len = pga[mapped].count;
                ost_pack_niobuf(&nioptr, pga[mapped].off, pga[mapped].count,
                                pga[mapped].flag, 0);
        }

        size[1] = page_count * sizeof(*remote);
        request->rq_replen = lustre_msg_size(2, size);
        rc = ptlrpc_queue_wait(request);
        rc = ptlrpc_check_status(request, rc);
        if (rc)
                GOTO(out_unmap, rc);

        nioptr = lustre_msg_buf(request->rq_repmsg, 1);
        if (!nioptr)
                GOTO(out_unmap, rc = -EINVAL);

        if (request->rq_repmsg->buflens[1] != size[1]) {
                CERROR("buffer length wrong (%d vs. %d)\n",
                       request->rq_repmsg->buflens[1], size[1]);
                GOTO(out_unmap, rc = -EINVAL);
        }

        for (j = 0; j < page_count; j++) {
                struct ptlrpc_bulk_page *bulk;

                ost_unpack_niobuf(&nioptr, &remote);

                bulk = ptlrpc_prep_bulk_page(desc);
                if (!bulk)
                        GOTO(out_unmap, rc = -ENOMEM);

                bulk->b_buf = (void *)(unsigned long)local[j].addr;
                bulk->b_buflen = local[j].len;
                bulk->b_xid = remote->xid;
                bulk->b_page = pga[j].pg;
        }

        if (desc->b_page_count != page_count)
                LBUG();

        /* Our reference is released when brw_finish is complete. */
        rc = ptlrpc_send_bulk(desc);

        /* XXX: Mike, same question as in osc_brw_read. */
        if (rc)
                GOTO(out_req, rc);

        /* Callbacks cause asynchronous handling. */
        rc = callback(data, 0, CB_PHASE_START);

        EXIT;
out_req:
        ptlrpc_req_finished(request);
        return rc;

        /* Clean up on error. */
out_unmap:
        while (mapped-- > 0)
                kunmap(pagearray[mapped]);

        OBD_FREE(local, page_count * sizeof(*local));
out_cb:
        OBD_FREE(cb_data, sizeof(*cb_data));
out_desc:
        ptlrpc_bulk_decref(desc);
        goto out_req;
}

static int osc_brw(int cmd, struct lustre_handle *conn,
                   struct lov_stripe_md *md, obd_count page_count,
                   struct brw_page *pga, brw_callback_t callback,
                   struct io_cb_data *data)
{
        if (cmd & OBD_BRW_WRITE)
                return osc_brw_write(conn, md, page_count, pga, callback, data);
        else
                return osc_brw_read(conn, md, page_count, pga, callback, data);
}

static int osc_enqueue(struct lustre_handle *connh, struct lov_stripe_md *md, 
                       struct lustre_handle *parent_lock, 
                       __u32 type, void *extentp, int extent_len, __u32 mode,
                       int *flags, void *callback, void *data, int datalen,
                       struct lustre_handle *lockh)
{
        __u64 res_id = { md->lmd_object_id };
        struct obd_device *obddev = class_conn2obd(connh);
        struct ldlm_extent *extent = extentp;
        int rc;
        __u32 mode2;

        /* Filesystem locks are given a bit of special treatment: first we
         * fixup the lock to start and end on page boundaries. */
        extent->start &= PAGE_MASK;
        extent->end = (extent->end + PAGE_SIZE - 1) & PAGE_MASK;

        /* Next, search for already existing extent locks that will cover us */
        //osc_con2dlmcl(conn, &cl, &connection, &rconn);
        rc = ldlm_lock_match(obddev->obd_namespace, &res_id, type, extent,
                             sizeof(extent), mode, lockh);
        if (rc == 1) {
                /* We already have a lock, and it's referenced */
                return 0;
        }

        /* Next, search for locks that we can upgrade (if we're trying to write)
         * or are more than we need (if we're trying to read).  Because the VFS
         * and page cache already protect us locally, lots of readers/writers
         * can share a single PW lock. */
        if (mode == LCK_PW)
                mode2 = LCK_PR;
        else
                mode2 = LCK_PW;

        rc = ldlm_lock_match(obddev->obd_namespace, &res_id, type, extent,
                             sizeof(extent), mode2, lockh);
        if (rc == 1) {
                int flags;
                /* FIXME: This is not incredibly elegant, but it might
                 * be more elegant than adding another parameter to
                 * lock_match.  I want a second opinion. */
                ldlm_lock_addref(lockh, mode);
                ldlm_lock_decref(lockh, mode2);

                if (mode == LCK_PR)
                        return 0;

                rc = ldlm_cli_convert(lockh, mode, &flags);
                if (rc)
                        LBUG();

                return rc;
        }

        rc = ldlm_cli_enqueue(connh, NULL,obddev->obd_namespace,
                              parent_lock, &res_id, type, extent,
                              sizeof(extent), mode, flags, ldlm_completion_ast,
                              callback, data, datalen, lockh);
        return rc;
}

static int osc_cancel(struct lustre_handle *oconn, struct lov_stripe_md *md,
                      __u32 mode, struct lustre_handle *lockh)
{
        ENTRY;

        ldlm_lock_decref(lockh, mode);

        RETURN(0);
}

static int osc_statfs(struct lustre_handle *conn, struct statfs *sfs)
{
        struct ptlrpc_request *request;
        struct obd_statfs *osfs;
        int rc, size = sizeof(*osfs);
        ENTRY;

        request = ptlrpc_prep_req2(conn, OST_STATFS, 0, NULL, NULL);
        if (!request)
                RETURN(-ENOMEM);

        request->rq_replen = lustre_msg_size(1, &size);

        rc = ptlrpc_queue_wait(request);
        rc = ptlrpc_check_status(request, rc);
        if (rc) {
                CERROR("%s failed: rc = %d\n", __FUNCTION__, rc);
                GOTO(out, rc);
        }

        osfs = lustre_msg_buf(request->rq_repmsg, 0);
        obd_statfs_unpack(osfs, sfs);

        EXIT;
 out:
        ptlrpc_free_req(request);
        return rc;
}

static int osc_iocontrol(long cmd, struct lustre_handle *conn, int len,
                         void *karg, void *uarg)
{
        struct obd_device *obddev = class_conn2obd(conn);
        struct obd_ioctl_data *data = karg;
        int err = 0;
        ENTRY;

        if (_IOC_TYPE(cmd) != IOC_LDLM_TYPE || _IOC_NR(cmd) < 
                        IOC_LDLM_MIN_NR || _IOC_NR(cmd) > IOC_LDLM_MAX_NR) {
                CDEBUG(D_IOCTL, "invalid ioctl (type %ld, nr %ld, size %ld)\n",
                        _IOC_TYPE(cmd), _IOC_NR(cmd), _IOC_SIZE(cmd));
                RETURN(-EINVAL);
        }

        switch (cmd) {
        case IOC_LDLM_TEST: {
                err = ldlm_test(obddev, conn);
                CERROR("-- done err %d\n", err);
                GOTO(out, err);
        }
        case IOC_LDLM_REGRESS_START: {
                unsigned int numthreads; 
                
                if (data->ioc_inllen1) 
                        numthreads = simple_strtoul(data->ioc_inlbuf1, NULL, 0);
                else 
                        numthreads = 1;

                err = ldlm_regression_start(obddev, conn, numthreads);
                CERROR("-- done err %d\n", err);
                GOTO(out, err);
        }
        case IOC_LDLM_REGRESS_STOP: {
                err = ldlm_regression_stop();
                CERROR("-- done err %d\n", err);
                GOTO(out, err);
        }
        default:
                GOTO(out, err = -EINVAL);
        }
out:
        return err;
}

struct obd_ops osc_obd_ops = {
        o_setup:        client_obd_setup,
        o_cleanup:      client_obd_cleanup,
        o_statfs:       osc_statfs,
        o_create:       osc_create,
        o_destroy:      osc_destroy,
        o_getattr:      osc_getattr,
        o_setattr:      osc_setattr,
        o_open:         osc_open,
        o_close:        osc_close,
        o_connect:      client_obd_connect,
        o_disconnect:   client_obd_disconnect,
        o_brw:          osc_brw,
        o_punch:        osc_punch,
        o_enqueue:      osc_enqueue,
        o_cancel:       osc_cancel,
        o_iocontrol:    osc_iocontrol
};

static int __init osc_init(void)
{
        return class_register_type(&osc_obd_ops, LUSTRE_OSC_NAME);
}

static void __exit osc_exit(void)
{
        class_unregister_type(LUSTRE_OSC_NAME);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Object Storage Client (OSC) v1.0");
MODULE_LICENSE("GPL");

module_init(osc_init);
module_exit(osc_exit);
