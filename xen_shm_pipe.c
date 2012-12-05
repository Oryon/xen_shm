/*
 * Xen shared memory pipe
 *
 * Authors: Vincent Brillault <git@lerya.net>
 *          Pierre Pfister    <oryon@darou.fr>
 *
 * This file contains the code and private headers of the
 * Xen shared memory pipe tool. See the headers file for
 * precisions about this module.
 *
 */
#include <stdio.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "xen_shm.h"

#define XEN_SHM_PIPE_PAGE_SIZE 4096 //Todo, find an interface


/* Private data about the pipe */
struct xen_shm_pipe_priv {
    int fd;
    xen_shm_pipe_mod mod;
    xen_shm_pipe_mod conv;

    struct xen_shm_pipe_shared* shared;

    size_t buffer_size;


};

/* Structure of the shared area */
struct xen_shm_pipe_shared {
    uint32_t write;
    uint32_t read;
    uint8_t buffer[1];
};

int inline
__xen_shm_pipe_is_offerer(struct xen_shm_pipe_priv* p)
{
    return (p->mod==read && p->conv==reader_offers) || (p->mod==write && p->conv==writer_offers);
}


int
__xen_shm_pipe_map_shared_memory(struct xen_shm_pipe_priv* p, uint8_t page_count)
{
    void* shared;

    shared = mmap(0, page_count*XEN_SHM_PIPE_PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, p->fd, 0);
    if (shared == MAP_FAILED) {
        return errno;
    }

    p->shared = shared;
    return 0;
}

int
xen_shm_pipe_init(xen_shm_pipe_p * pipe,xen_shm_pipe_mod mod,xen_shm_pipe_conv conv)
{
    struct xen_shm_pipe_priv* p = malloc(sizeof(struct xen_shm_pipe_priv));
    int retval;

    if(p==NULL)
        return ENOMEM;

    p->fd = open(XEN_SHM_DEVICE_PATH , O_RDWR);
    if (p->fd < 0) {
       free(p);
       return ENODEV;
    }

    p->conv = conv;
    p->mod = mod;
    p->shared = NULL;
    *pipe = p;

    return 0;

}

int xen_shm_pipe_getdomid(xen_shm_pipe_p pipe, uint32_t* receiver_domid) {
    struct xen_shm_pipe_priv* p;
    struct xen_shm_ioctlarg_getdomid getdomid;
    int retval;

    p = pipe;
    if(__xen_shm_pipe_is_offerer(p)) {
        return EINVAL;
    }

    retval = ioctl(p->fd, XEN_SHM_IOCTL_GET_DOMID, &getdomid);
    if (retval != 0) {
        return retval;
    }

    *receiver_domid = getdomid.local_domid;

    return 0;
}

int
xen_shm_pipe_offers(xen_shm_pipe_p pipe, uint8_t page_count,
        uint32_t receiver_domid, uint32_t* offerer_domid, uint32_t* grant_ref)
{
    struct xen_shm_pipe_priv* p;
    struct xen_shm_ioctlarg_offerer init_offerer;
    int retval;

    p = pipe;
    if(!__xen_shm_pipe_is_offerer(p)) {
        return EINVAL;
    }

    init_offerer.pages_count = page_count;
    init_offerer.dist_domid = receiver_domid;

    retval = ioctl(p->fd, XEN_SHM_IOCTL_INIT_OFFERER, &init_offerer);
    if (retval != 0) {
        return retval;
    }


    retval = __xen_shm_pipe_map_shared_memory(p, page_count);
    if(retval != 0) {
        return retval;
    }

    *offerer_domid = init_offerer.local_domid;
    *grant_ref = init_offerer.grant;
    p->buffer_size = page_count*XEN_SHM_PIPE_PAGE_SIZE - sizeof(struct xen_shm_pipe_shared) + 1;

    return 0;
}


int
xen_shm_pipe_connect(xen_shm_pipe_p pipe, uint8_t page_count, uint32_t offerer_domid, uint32_t grant_ref)
{
    struct xen_shm_pipe_priv* p;
    struct xen_shm_ioctlarg_receiver init_receiver;
    int retval;

    p = pipe;
    if(__xen_shm_pipe_is_offerer(p)) {
        return EINVAL;
    }

    init_receiver.pages_count = page_count;
    init_receiver.dist_domid = offerer_domid;
    init_receiver.grant = grant_ref;

    retval = ioctl(p->fd, XEN_SHM_IOCTL_INIT_RECEIVER, &init_receiver);
    if (retval != 0) {
        return retval;
    }

    retval = __xen_shm_pipe_map_shared_memory(p, page_count);
    if(retval != 0) {
        return retval;
    }

    p->buffer_size = page_count*XEN_SHM_PIPE_PAGE_SIZE - sizeof(struct xen_shm_pipe_shared) + 1;

    return 0;
}

int xen_shm_pipe_wait(xen_shm_pipe_p pipe, unsigned long timeout_ms) {
    struct xen_shm_pipe_priv* p;
    struct xen_shm_ioctlarg_await wait;
    int retval;

    p = pipe;
    if(!__xen_shm_pipe_is_offerer(p) || p->shared == NULL) {
        return EINVAL;
    }

    wait.request_flags = XEN_SHM_IOCTL_AWAIT_INIT;
    wait.timeout_ms = timeout_ms;

    return ioctl(p->fd, XEN_SHM_IOCTL_AWAIT, &wait);
}


int xen_shm_pipe_free(xen_shm_pipe_p pipe) {
    struct xen_shm_pipe_priv* p;

    p = pipe;
    if(p->shared !=NULL) {
        munmap(p->shared, p->buffer_size + sizeof(struct xen_shm_pipe_shared) - 1);
    }

    fclose(p->fd);
    free(pipe);
}




