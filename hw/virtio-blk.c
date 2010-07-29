/*
 * Virtio Block Device
 *
 * Copyright IBM, Corp. 2007
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include <pthread.h>
#include <libaio.h>
#include "qemu-common.h"
#include "virtio-blk.h"
#include "hw/dataplane/event-poll.h"
#include "hw/dataplane/vring.h"
#include "hw/dataplane/ioq.h"
#include "hw/dataplane/iosched.h"
#include "kvm.h"

enum {
    SEG_MAX = 126,                  /* maximum number of I/O segments */
    VRING_MAX = SEG_MAX + 2,        /* maximum number of vring descriptors */
    REQ_MAX = VRING_MAX,            /* maximum number of requests in the vring,
                                     * is VRING_MAX / 2 with traditional and
                                     * VRING_MAX with indirect descriptors */
};

/** I/O request
 *
 * Most I/O requests need to know the vring index (head) and the completion
 * status byte that must be filled in to tell the guest whether or not the
 * request succeeded.
 *
 * The iovec array pointed to by the iocb is valid only before ioq_submit() is
 * called.  After that, neither the kernel nor userspace needs to access the
 * iovec anymore and the memory is no longer owned by this VirtIOBlockRequest.
 *
 * Requests can be merged together by the I/O scheduler.  When this happens,
 * the next_merged field is used to link the requests and only the first
 * request's iocb is used.  Merged requests require memory allocation for the
 * iovec array and must be freed appropriately.
 */
typedef struct VirtIOBlockRequest VirtIOBlockRequest;
struct VirtIOBlockRequest {
    struct iocb iocb;               /* Linux AIO control block */
    unsigned char *status;          /* virtio block status code */
    unsigned int head;              /* vring descriptor index */
    int len;                        /* number of I/O bytes, only used for merged reqs */
    VirtIOBlockRequest *next_merged;/* next merged iocb or NULL */
};

typedef struct {
    VirtIODevice vdev;
    BlockDriverState *bs;
    VirtQueue *vq;
    BlockConf *conf;
    unsigned short sector_mask;
    char sn[BLOCK_SERIAL_STRLEN];

    bool data_plane_started;
    pthread_t data_plane_thread;

    Vring vring;                    /* virtqueue vring */

    EventPoll event_poll;           /* event poller */
    EventHandler io_handler;        /* Linux AIO completion handler */
    EventHandler notify_handler;    /* virtqueue notify handler */

    IOQueue ioqueue;                /* Linux AIO queue (should really be per dataplane thread) */
    IOSched iosched;                /* I/O scheduler */
    VirtIOBlockRequest requests[REQ_MAX]; /* pool of requests, managed by the queue */
} VirtIOBlock;

static VirtIOBlock *to_virtio_blk(VirtIODevice *vdev)
{
    return (VirtIOBlock *)vdev;
}

/* Normally the block driver passes down the fd, there's no way to get it from
 * above.
 */
static int get_raw_posix_fd_hack(VirtIOBlock *s)
{
    return *(int*)s->bs->file->opaque;
}

/* Raise an interrupt to signal guest, if necessary */
static void virtio_blk_notify_guest(VirtIOBlock *s)
{
    /* Always notify when queue is empty (when feature acknowledge) */
	if ((s->vring.vr.avail->flags & VRING_AVAIL_F_NO_INTERRUPT) &&
	    (s->vring.vr.avail->idx != s->vring.last_avail_idx ||
        !(s->vdev.guest_features & (1 << VIRTIO_F_NOTIFY_ON_EMPTY))))
		return;

    /* Try to issue the ioctl() directly for speed */
    if (likely(virtio_queue_try_notify_from_thread(s->vq))) {
        return;
    }

    /* If the fast path didn't work, use irqfd */
    event_notifier_set(virtio_queue_get_guest_notifier(s->vq));
}

static void complete_one_request(VirtIOBlockRequest *req, VirtIOBlock *s, ssize_t ret)
{
    int len;

    if (likely(ret >= 0)) {
        *req->status = VIRTIO_BLK_S_OK;

        /* Merged requests know their part of the length, single requests can
         * just use the return value.
         */
        len = unlikely(req->len) ? req->len : ret;
    } else {
        *req->status = VIRTIO_BLK_S_IOERR;
        len = 0;
    }

    /* According to the virtio specification len should be the number of bytes
     * written to, but for virtio-blk it seems to be the number of bytes
     * transferred plus the status bytes.
     */
    vring_push(&s->vring, req->head, len + sizeof req->status);
}

static bool is_request_merged(VirtIOBlockRequest *req)
{
    return req->next_merged;
}

static void complete_request(struct iocb *iocb, ssize_t ret, void *opaque)
{
    VirtIOBlock *s = opaque;
    VirtIOBlockRequest *req = container_of(iocb, VirtIOBlockRequest, iocb);

    /* Free the iovec now, it isn't needed */
    if (unlikely(is_request_merged(req))) {
        qemu_free((void*)iocb->u.v.vec);
    }

    while (req) {
        complete_one_request(req, s, ret);

        VirtIOBlockRequest *next = req->next_merged;
        ioq_put_iocb(&s->ioqueue, &req->iocb);
        req = next;
    }
}

static void merge_request(struct iocb *iocb_a, struct iocb *iocb_b)
{
    /* Repeated merging could be made more efficient using realloc, but this
     * approach keeps it simple. */

    VirtIOBlockRequest *req_a = container_of(iocb_a, VirtIOBlockRequest, iocb);
    VirtIOBlockRequest *req_b = container_of(iocb_b, VirtIOBlockRequest, iocb);
    struct iovec *iovec = qemu_malloc((iocb_a->u.v.nr + iocb_b->u.v.nr) * sizeof iovec[0]);

    memcpy(iovec, iocb_a->u.v.vec, iocb_a->u.v.nr * sizeof iovec[0]);
    memcpy(iovec + iocb_a->u.v.nr, iocb_b->u.v.vec, iocb_b->u.v.nr * sizeof iovec[0]);

    if (is_request_merged(req_a)) {
        /* Free the old merged iovec */
        qemu_free((void*)iocb_a->u.v.vec);
    } else {
        /* Stash the request length */
        req_a->len = iocb_nbytes(iocb_a);
    }

    iocb_b->u.v.vec = iovec;
    req_b->len = iocb_nbytes(iocb_b);
    req_b->next_merged = req_a;
    /*
    fprintf(stderr, "merged %p (%u) and %p (%u), %u iovecs in total\n",
            req_a, iocb_a->u.v.nr, req_b, iocb_b->u.v.nr, iocb_a->u.v.nr + iocb_b->u.v.nr);
    */
}

static void process_request(IOQueue *ioq, struct iovec iov[], unsigned int out_num, unsigned int in_num, unsigned int head)
{
    /* Virtio block requests look like this: */
    struct virtio_blk_outhdr *outhdr; /* iov[0] */
    /* data[]                            ... */
    struct virtio_blk_inhdr *inhdr;   /* iov[out_num + in_num - 1] */

    if (unlikely(out_num == 0 || in_num == 0 ||
                iov[0].iov_len != sizeof *outhdr ||
                iov[out_num + in_num - 1].iov_len != sizeof *inhdr)) {
        fprintf(stderr, "virtio-blk invalid request\n");
        exit(1);
    }

    outhdr = iov[0].iov_base;
    inhdr = iov[out_num + in_num - 1].iov_base;

    /*
    fprintf(stderr, "virtio-blk request type=%#x sector=%#lx\n",
            outhdr->type, outhdr->sector);
    */

    /* TODO Linux sets the barrier bit even when not advertised! */
    uint32_t type = outhdr->type & ~VIRTIO_BLK_T_BARRIER;

    if (unlikely(type & ~(VIRTIO_BLK_T_OUT | VIRTIO_BLK_T_FLUSH))) {
        fprintf(stderr, "virtio-blk unsupported request type %#x\n", outhdr->type);
        exit(1);
    }

    struct iocb *iocb;
    switch (type & (VIRTIO_BLK_T_OUT | VIRTIO_BLK_T_FLUSH)) {
    case VIRTIO_BLK_T_IN:
        if (unlikely(out_num != 1)) {
            fprintf(stderr, "virtio-blk invalid read request\n");
            exit(1);
        }
        iocb = ioq_rdwr(ioq, true, &iov[1], in_num - 1, outhdr->sector * 512UL); /* TODO is it always 512? */
        break;

    case VIRTIO_BLK_T_OUT:
        if (unlikely(in_num != 1)) {
            fprintf(stderr, "virtio-blk invalid write request\n");
            exit(1);
        }
        iocb = ioq_rdwr(ioq, false, &iov[1], out_num - 1, outhdr->sector * 512UL); /* TODO is it always 512? */
        break;

    case VIRTIO_BLK_T_FLUSH:
        if (unlikely(in_num != 1 || out_num != 1)) {
            fprintf(stderr, "virtio-blk invalid flush request\n");
            exit(1);
        }

        /* TODO fdsync is not supported by all backends, do it synchronously here! */
        {
            VirtIOBlock *s = container_of(ioq, VirtIOBlock, ioqueue);
            fdatasync(get_raw_posix_fd_hack(s));
            inhdr->status = VIRTIO_BLK_S_OK;
            vring_push(&s->vring, head, sizeof *inhdr);
            virtio_blk_notify_guest(s);
        }
        return;

    default:
        fprintf(stderr, "virtio-blk multiple request type bits set\n");
        exit(1);
    }

    /* Fill in virtio block metadata needed for completion */
    VirtIOBlockRequest *req = container_of(iocb, VirtIOBlockRequest, iocb);
    req->head = head;
    req->status = &inhdr->status;
    req->len = 0;
    req->next_merged = NULL;
}

static bool handle_notify(EventHandler *handler)
{
    VirtIOBlock *s = container_of(handler, VirtIOBlock, notify_handler);

    /* There is one array of iovecs into which all new requests are extracted
     * from the vring.  Requests are read from the vring and the translated
     * descriptors are written to the iovecs array.  The iovecs do not have to
     * persist across handle_notify() calls because the kernel copies the
     * iovecs on io_submit().
     *
     * Handling io_submit() EAGAIN may require storing the requests across
     * handle_notify() calls until the kernel has sufficient resources to
     * accept more I/O.  This is not implemented yet.
     */
    struct iovec iovec[VRING_MAX];
    struct iovec *end = &iovec[VRING_MAX];
    struct iovec *iov = iovec;

    /* When a request is read from the vring, the index of the first descriptor
     * (aka head) is returned so that the completed request can be pushed onto
     * the vring later.
     *
     * The number of hypervisor read-only iovecs is out_num.  The number of
     * hypervisor write-only iovecs is in_num.
     */
    int head;
    unsigned int out_num = 0, in_num = 0;

    for (;;) {
        /* Disable guest->host notifies to avoid unnecessary vmexits */
        vring_disable_cb(&s->vring);

        for (;;) {
            head = vring_pop(&s->vring, iov, end, &out_num, &in_num);
            if (head < 0) {
                break; /* no more requests */
            }

            /*
            fprintf(stderr, "out_num=%u in_num=%u head=%d\n", out_num, in_num, head);
            */

            process_request(&s->ioqueue, iov, out_num, in_num, head);
            iov += out_num + in_num;
        }

        if (likely(head == -EAGAIN)) { /* vring emptied */
            /* Re-enable guest->host notifies and stop processing the vring.
             * But if the guest has snuck in more descriptors, keep processing.
             */
            if (likely(vring_enable_cb(&s->vring))) {
                break;
            }
        } else { /* head == -ENOBUFS, cannot continue since iovecs[] is depleted */
            /* Since there are no iovecs[] left, stop processing for now.  Do
             * not re-enable guest->host notifies since the I/O completion
             * handler knows to check for more vring descriptors anyway.
             */
            break;
        }
    }

    iosched(&s->iosched, s->ioqueue.queue, &s->ioqueue.queue_idx, merge_request);

    /* Submit requests, if any */
    int rc = ioq_submit(&s->ioqueue);
    if (unlikely(rc < 0)) {
        fprintf(stderr, "ioq_submit failed %d\n", rc);
        exit(1);
    }
    return true;
}

static bool handle_io(EventHandler *handler)
{
    VirtIOBlock *s = container_of(handler, VirtIOBlock, io_handler);

    if (ioq_run_completion(&s->ioqueue, complete_request, s) > 0) {
        virtio_blk_notify_guest(s);
    }

    /* If there were more requests than iovecs, the vring will not be empty yet
     * so check again.  There should now be enough resources to process more
     * requests.
     */
    if (unlikely(vring_more_avail(&s->vring))) {
        return handle_notify(&s->notify_handler);
    }

    return true;
}

static void *data_plane_thread(void *opaque)
{
    VirtIOBlock *s = opaque;

    event_poll_run(&s->event_poll);
    return NULL;
}

static void data_plane_start(VirtIOBlock *s)
{
    int i;

    iosched_init(&s->iosched);
    vring_setup(&s->vring, &s->vdev, 0);

    /* Set up guest notifier (irq) */
    if (s->vdev.binding->set_guest_notifier(s->vdev.binding_opaque, 0, true) != 0) {
        fprintf(stderr, "virtio-blk failed to set guest notifier\n");
        exit(1);
    }

    event_poll_init(&s->event_poll);

    /* Set up virtqueue notify */
    if (s->vdev.binding->set_host_notifier(s->vdev.binding_opaque, 0, true) != 0) {
        fprintf(stderr, "virtio-blk failed to set host notifier, ensure -enable-kvm is set\n");
        exit(1);
    }
    event_poll_add(&s->event_poll, &s->notify_handler,
                   virtio_queue_get_host_notifier(s->vq),
                   handle_notify);

    /* Set up ioqueue */
    ioq_init(&s->ioqueue, get_raw_posix_fd_hack(s), REQ_MAX);
    for (i = 0; i < ARRAY_SIZE(s->requests); i++) {
        ioq_put_iocb(&s->ioqueue, &s->requests[i].iocb);
    }
    event_poll_add(&s->event_poll, &s->io_handler, ioq_get_notifier(&s->ioqueue), handle_io);

    /* Create data plane thread */
    sigset_t set, oldset;
    sigfillset(&set);
    pthread_sigmask(SIG_SETMASK, &set, &oldset);
    if (pthread_create(&s->data_plane_thread, NULL, data_plane_thread, s) != 0)
    {
        fprintf(stderr, "pthread create failed: %m\n");
        exit(1);
    }
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);

    s->data_plane_started = true;
}

static void data_plane_stop(VirtIOBlock *s)
{
    s->data_plane_started = false;

    /* Tell data plane thread to stop and then wait for it to return */
    event_poll_stop(&s->event_poll);
    pthread_join(s->data_plane_thread, NULL);

    ioq_cleanup(&s->ioqueue);

    s->vdev.binding->set_host_notifier(s->vdev.binding_opaque, 0, false);

    event_poll_cleanup(&s->event_poll);

    /* Clean up guest notifier (irq) */
    s->vdev.binding->set_guest_notifier(s->vdev.binding_opaque, 0, false);
}

static void virtio_blk_set_status(VirtIODevice *vdev, uint8_t val)
{
    VirtIOBlock *s = to_virtio_blk(vdev);

    /* Toggle host notifier only on status change */
    if (s->data_plane_started == !!(val & VIRTIO_CONFIG_S_DRIVER_OK)) {
        return;
    }

    /*
    fprintf(stderr, "virtio_blk_set_status %#x\n", val);
    */

    if (val & VIRTIO_CONFIG_S_DRIVER_OK) {
        data_plane_start(s);
    } else {
        data_plane_stop(s);
    }
}

static void virtio_blk_reset(VirtIODevice *vdev)
{
    virtio_blk_set_status(vdev, 0);
}

static void virtio_blk_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBlock *s = to_virtio_blk(vdev);

    if (s->data_plane_started) {
        fprintf(stderr, "virtio_blk_handle_output: should never get here, "
                        "data plane thread should process requests\n");
        exit(1);
    }

    /* Linux seems to notify before the driver comes up.  This needs more
     * investigation.  Just use a hack for now.
     */
    virtio_blk_set_status(vdev, VIRTIO_CONFIG_S_DRIVER_OK); /* start the thread */

    /* Now kick the thread */
    event_notifier_set(virtio_queue_get_host_notifier(s->vq));
}

/* coalesce internal state, copy to pci i/o region 0
 */
static void virtio_blk_update_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOBlock *s = to_virtio_blk(vdev);
    struct virtio_blk_config blkcfg;
    uint64_t capacity;
    int cylinders, heads, secs;

    bdrv_get_geometry(s->bs, &capacity);
    bdrv_get_geometry_hint(s->bs, &cylinders, &heads, &secs);
    memset(&blkcfg, 0, sizeof(blkcfg));
    stq_raw(&blkcfg.capacity, capacity);
    stl_raw(&blkcfg.seg_max, SEG_MAX);
    stw_raw(&blkcfg.cylinders, cylinders);
    blkcfg.heads = heads;
    blkcfg.sectors = secs & ~s->sector_mask;
    blkcfg.blk_size = s->conf->logical_block_size;
    blkcfg.size_max = 0;
    blkcfg.physical_block_exp = get_physical_block_exp(s->conf);
    blkcfg.alignment_offset = 0;
    blkcfg.min_io_size = s->conf->min_io_size / blkcfg.blk_size;
    blkcfg.opt_io_size = s->conf->opt_io_size / blkcfg.blk_size;
    memcpy(config, &blkcfg, sizeof(struct virtio_blk_config));
}

static uint32_t virtio_blk_get_features(VirtIODevice *vdev, uint32_t features)
{
    VirtIOBlock *s = to_virtio_blk(vdev);

    features |= (1 << VIRTIO_BLK_F_SEG_MAX);
    features |= (1 << VIRTIO_BLK_F_GEOMETRY);
    features |= (1 << VIRTIO_BLK_F_TOPOLOGY);
    features |= (1 << VIRTIO_BLK_F_BLK_SIZE);

    if (bdrv_enable_write_cache(s->bs))
        features |= (1 << VIRTIO_BLK_F_WCACHE);
    
    if (bdrv_is_read_only(s->bs))
        features |= 1 << VIRTIO_BLK_F_RO;

    return features;
}

VirtIODevice *virtio_blk_init(DeviceState *dev, BlockConf *conf)
{
    VirtIOBlock *s;
    int cylinders, heads, secs;
    DriveInfo *dinfo;

    s = (VirtIOBlock *)virtio_common_init("virtio-blk", VIRTIO_ID_BLOCK,
                                          sizeof(struct virtio_blk_config),
                                          sizeof(VirtIOBlock));

    s->vdev.get_config = virtio_blk_update_config;
    s->vdev.get_features = virtio_blk_get_features;
    s->vdev.set_status = virtio_blk_set_status;
    s->vdev.reset = virtio_blk_reset;
    s->bs = conf->bs;
    s->conf = conf;
    s->sector_mask = (s->conf->logical_block_size / BDRV_SECTOR_SIZE) - 1;
    bdrv_guess_geometry(s->bs, &cylinders, &heads, &secs);

    /* NB: per existing s/n string convention the string is terminated
     * by '\0' only when less than sizeof (s->sn)
     */
    dinfo = drive_get_by_blockdev(s->bs);
    strncpy(s->sn, dinfo->serial, sizeof (s->sn));

    s->vq = virtio_add_queue(&s->vdev, VRING_MAX, virtio_blk_handle_output);
    s->data_plane_started = false;

    bdrv_set_removable(s->bs, 0);

    return &s->vdev;
}
