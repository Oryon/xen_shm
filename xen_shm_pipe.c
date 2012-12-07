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
#include <stdlib.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stddef.h>

#include "xen_shm_pipe.h"
#include "xen_shm.h"

#define XEN_SHM_PIPE_PAGE_SIZE 4096 //Todo, find an interface


#define XEN_SHM_PIPE_INITIAL_WAIT_CHECK_INTERVAL 128 //Will check for the other after the first <value> written or read values (for better delays, it is a small value)
#define XEN_SHM_PIPE_WAIT_CHECK_PER_ROUND 4 //Minimal number of time the writer/read checks the otherone state while writing/reading

/* Different reader/writer flags */
#define XSHMP_OPENED  0x00000001u
#define XSHMP_CLOSED  0x00000002u
#define XSHMP_WAITING 0x00000004u

/* Private data about the pipe */
struct xen_shm_pipe_priv {
    int fd;
    enum xen_shm_pipe_mod mod;
    enum xen_shm_pipe_conv conv;

    struct xen_shm_pipe_shared* shared;

    size_t buffer_size;
    ptrdiff_t wait_check_interval;

};

/* Structure of the shared area */
struct xen_shm_pipe_shared {
    uint32_t writer_flags;
    uint32_t reader_flags;
    uint32_t write;
    uint32_t read;
    uint8_t buffer[0];
};

inline int __xen_shm_pipe_is_offerer(struct xen_shm_pipe_priv* p);
int __xen_shm_pipe_map_shared_memory(struct xen_shm_pipe_priv* p, uint8_t page_count);
uint32_t* __xen_shm_pipe_get_flags(struct xen_shm_pipe_priv* p, int my_flags);


inline int
__xen_shm_pipe_is_offerer(struct xen_shm_pipe_priv* p)
{
    return (p->mod==xen_shm_pipe_mod_read && p->conv==xen_shm_pipe_conv_reader_offers)
            || (p->mod==xen_shm_pipe_mod_write && p->conv==xen_shm_pipe_conv_writer_offers);
}

uint32_t*
__xen_shm_pipe_get_flags(struct xen_shm_pipe_priv* p, int my_flags)
{
    if(p->mod==xen_shm_pipe_mod_write) {
        return (my_flags)?&(p->shared->writer_flags):&(p->shared->reader_flags);
    } else {
        return (my_flags)?&(p->shared->reader_flags):&(p->shared->writer_flags);
    }
}

int
__xen_shm_pipe_map_shared_memory(struct xen_shm_pipe_priv* p, uint8_t page_count)
{
    void* shared;

    shared = mmap(0, (size_t) page_count*XEN_SHM_PIPE_PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, p->fd, 0);
    if (shared == MAP_FAILED) {
        return -1;
    }

    p->shared = shared;
    return 0;
}

int
xen_shm_pipe_init(xen_shm_pipe_p * xpipe,enum xen_shm_pipe_mod mod,enum xen_shm_pipe_conv conv)
{
    struct xen_shm_pipe_priv* p = malloc(sizeof(struct xen_shm_pipe_priv));

    if(p==NULL) {
        errno = ENOMEM;
        return -1;
    }

    p->fd = open(XEN_SHM_DEVICE_PATH , O_RDWR);
    if (p->fd < 0) {
       free(p);
       errno = ENODEV;
       return -1;
    }

    p->conv = conv;
    p->mod = mod;
    p->shared = NULL;
    *xpipe = p;

    return 0;

}

int xen_shm_pipe_getdomid(xen_shm_pipe_p xpipe, uint32_t* receiver_domid) {
    struct xen_shm_pipe_priv* p;
    struct xen_shm_ioctlarg_getdomid getdomid;

    p = xpipe;
    if(__xen_shm_pipe_is_offerer(p)) {
        errno = EMEDIUMTYPE;
        return -1;
    }


    if (ioctl(p->fd, XEN_SHM_IOCTL_GET_DOMID, &getdomid)) {
        return -1;
    }

    *receiver_domid = getdomid.local_domid;

    return 0;
}

int
xen_shm_pipe_offers(xen_shm_pipe_p xpipe, uint8_t page_count,
        uint32_t receiver_domid, uint32_t* offerer_domid, uint32_t* grant_ref)
{
    struct xen_shm_pipe_priv* p;
    struct xen_shm_ioctlarg_offerer init_offerer;


    p = xpipe;
    if(!__xen_shm_pipe_is_offerer(p)) {
        errno = EMEDIUMTYPE;
        return -1;
    }

    init_offerer.pages_count = page_count;
    init_offerer.dist_domid = (domid_t) receiver_domid;

    if (ioctl(p->fd, XEN_SHM_IOCTL_INIT_OFFERER, &init_offerer)) {
        return -1;
    }


    if(__xen_shm_pipe_map_shared_memory(p, page_count)) {
        return -1;
    }

    *offerer_domid = (uint32_t) init_offerer.local_domid;
    *grant_ref = (uint32_t) init_offerer.grant;
    p->buffer_size = (size_t) page_count*XEN_SHM_PIPE_PAGE_SIZE - sizeof(struct xen_shm_pipe_shared);
    p->wait_check_interval = ((ptrdiff_t) p->buffer_size)/XEN_SHM_PIPE_WAIT_CHECK_PER_ROUND;
    //init structure
    p->shared->reader_flags = 0;
    p->shared->writer_flags = 0;
    p->shared->read = 0;
    p->shared->write = 0;

    //Set my flag to open
    uint32_t* myflags = __xen_shm_pipe_get_flags(p, 1);
    *myflags |= XSHMP_OPENED;

    return 0;
}


int
xen_shm_pipe_connect(xen_shm_pipe_p xpipe, uint8_t page_count, uint32_t offerer_domid, uint32_t grant_ref)
{
    struct xen_shm_pipe_priv* p;
    struct xen_shm_ioctlarg_receiver init_receiver;

    p = xpipe;
    if(__xen_shm_pipe_is_offerer(p)) {
        errno = EMEDIUMTYPE;
        return -1;
    }

    init_receiver.pages_count = page_count;
    init_receiver.dist_domid = (domid_t) offerer_domid;
    init_receiver.grant = grant_ref;

    if (ioctl(p->fd, XEN_SHM_IOCTL_INIT_RECEIVER, &init_receiver)) {
        return -1;
    }


    if(__xen_shm_pipe_map_shared_memory(p, page_count)) {
        return -1;
    }

    p->buffer_size = (size_t) page_count*XEN_SHM_PIPE_PAGE_SIZE - sizeof(struct xen_shm_pipe_shared);
    p->wait_check_interval = ((ptrdiff_t) p->buffer_size)/XEN_SHM_PIPE_WAIT_CHECK_PER_ROUND;
    //Set my flag to open
    uint32_t* myflags = __xen_shm_pipe_get_flags(p, 1);
    *myflags |= XSHMP_OPENED;

    return 0;
}

int xen_shm_pipe_wait(xen_shm_pipe_p xpipe, unsigned long timeout_ms) {
    struct xen_shm_pipe_priv* p;
    struct xen_shm_ioctlarg_await wait;

    p = xpipe;
    if(!__xen_shm_pipe_is_offerer(p) || p->shared == NULL) {
        errno = EMEDIUMTYPE;
        return -1;
    }

    wait.request_flags = XEN_SHM_IOCTL_AWAIT_INIT;
    wait.timeout_ms = timeout_ms;

    if(ioctl(p->fd, XEN_SHM_IOCTL_AWAIT, &wait)) {
        return -1;
    }

    if(wait.remaining_ms==0) {
        errno = ETIME;
        return -1;
    }

    return 0;

}


void xen_shm_pipe_free(xen_shm_pipe_p xpipe) {
    struct xen_shm_pipe_priv* p;

    p = xpipe;
    if(p->shared !=NULL) {
        uint32_t* myflags = __xen_shm_pipe_get_flags(p, 1);
        *myflags |= XSHMP_CLOSED;

        munmap(p->shared, p->buffer_size + sizeof(struct xen_shm_pipe_shared));
    }

    close(p->fd);
    free(xpipe);
}



ssize_t
xen_shm_pipe_read(xen_shm_pipe_p xpipe, void* buf, size_t nbytes)
{
    struct xen_shm_pipe_priv* p;
    struct xen_shm_pipe_shared* s;
    struct xen_shm_ioctlarg_await await_op;
    int ioctl_ret;

    uint8_t* user_buf; //A cast of the given user buffer

    uint8_t* read_pos; //Read pointer in circular buffer
    uint8_t* write_pos;//Write pointer in circ buff
    uint8_t* shared_max; //Out of bound pointer in circ buffer

    uint8_t* current_buf;//Current write pos

    uint8_t* usr_max_buf;//Out of bound given by the user
    uint8_t* gran_max_buf;//Out of bound to check if the writer is waiting
    uint8_t* circ_max_buf;//Out of bound given by the circ buffer

    uint8_t* min_max_buf;//Min value of the 3 different out of bound

    p = xpipe;
    if(p->mod == xen_shm_pipe_mod_write) { //Not reader
        errno = EMEDIUMTYPE;
        return -1;
    }

    if(p->shared == NULL) { //Not initialized
        errno = EMEDIUMTYPE;
        return -1;
    }

    s = p->shared;
    if(s->reader_flags & XSHMP_CLOSED) {//Closed
        return 0;
    }

    //Prepare wait structure
    await_op.request_flags = XEN_SHM_IOCTL_AWAIT_USER;
    await_op.timeout_ms = 0;

    shared_max = s->buffer + (ptrdiff_t) p->buffer_size;

    read_pos = s->buffer + (ptrdiff_t) s->read ;
    write_pos = s->buffer + (ptrdiff_t) s->write ;

    //Wait for available data
    while(read_pos == write_pos) {
        s->reader_flags |= XSHMP_WAITING; //Notify that we are waiting

        if(s->writer_flags & XSHMP_CLOSED){ //Maybe it was gracefully closed
            s->reader_flags &= ~XSHMP_WAITING; //Stop waiting
            return 0;
        }

        ioctl_ret = ioctl(p->fd, XEN_SHM_IOCTL_AWAIT, &await_op);

        read_pos = s->buffer + (ptrdiff_t) s->read ;
        write_pos = s->buffer + (ptrdiff_t) s->write ;

        if(ioctl_ret) {
            if(errno == EPIPE) { //Other side is not here
                if(read_pos == write_pos) { //We read everything
                    s->reader_flags &= ~XSHMP_WAITING; //Stop waiting
                    return (s->writer_flags & XSHMP_CLOSED)?0:-1;
                } //Ignore error and read
                break;
            }//True error (like interrupt)
            s->reader_flags &= ~XSHMP_WAITING; //Stop waiting
            return -1;
        }

    }
    s->reader_flags &= ~XSHMP_WAITING; //Stop waiting

    user_buf = (uint8_t*) buf;

    current_buf = user_buf;
    usr_max_buf = user_buf + (ptrdiff_t) nbytes;

    gran_max_buf = user_buf + (ptrdiff_t) XEN_SHM_PIPE_INITIAL_WAIT_CHECK_INTERVAL;
    circ_max_buf = user_buf;
    min_max_buf = user_buf;

    while(read_pos != write_pos && current_buf != usr_max_buf) //We read as much as we can
    {

        /*
         * Get the minimum of different boundaries
         */
        min_max_buf = circ_max_buf;
        if(min_max_buf > usr_max_buf) {
            min_max_buf = usr_max_buf;
        }
        if(min_max_buf > gran_max_buf) {
            min_max_buf = gran_max_buf;
        }

        /*
         * Actually read
         */
        while(current_buf != min_max_buf) {
            *current_buf = *read_pos;
            ++current_buf;
            ++read_pos;
        }

        /*
         * Check boundary values
         */
        if(current_buf == gran_max_buf) { //Time to take news of the other guy
            if(s->writer_flags & XSHMP_WAITING) { //Writer is waiting
                ioctl(p->fd, XEN_SHM_IOCTL_SSIG, 0);
            }
            gran_max_buf += p->wait_check_interval;
        }

        if(read_pos == shared_max) { //Time to check if the read pointer must be rewind
            read_pos = s->buffer;
        }

        write_pos = s->buffer + (ptrdiff_t) s->write; //Updates write_pos value (it could have changed)
        s->read = (uint32_t) (read_pos - s->buffer); //Update read position in shared memory

        if(read_pos <= write_pos) { //Updates the circ buffer threshold
            circ_max_buf = current_buf + (ptrdiff_t)(write_pos - read_pos);
        } else {
            circ_max_buf = current_buf +  (ptrdiff_t)(shared_max - read_pos);
        }


    }

    if(s->writer_flags & XSHMP_WAITING) { //Writer is waiting
        ioctl(p->fd, XEN_SHM_IOCTL_SSIG, 0);
    }


    return (ssize_t) (current_buf - user_buf);
}



ssize_t xen_shm_pipe_write(xen_shm_pipe_p xpipe, const void* buf, size_t nbytes) {
    struct xen_shm_pipe_priv* p;
    struct xen_shm_pipe_shared* s;
    struct xen_shm_ioctlarg_await await_op;

    uint8_t* read_pos_reduced; //Read pointer - 1 in circular buffer
    uint8_t* write_pos;//Write pointer in circ buff
    uint8_t* shared_max; //Out of bound pointer in circ buffer

    const uint8_t* user_buf; //A cast of the given user buffer
    const uint8_t* current_buf;//Current write pos

    const uint8_t* usr_max_buf;//Out of bound given by the user
    const uint8_t* gran_max_buf;//Out of bound to check if the reader is waiting
    const uint8_t* circ_max_buf;//Out of bound given by the circ buffer

    const uint8_t* min_max_buf;//Min value of the 3 different out of bound

    p = xpipe;
    if(p->mod == xen_shm_pipe_mod_read) { //Not reader
        errno = EMEDIUMTYPE;
        return -1;
    }

    if(p->shared == NULL) { //Not initialized
        errno = EMEDIUMTYPE;
        return -1;
    }

    s = p->shared;
    if(s->writer_flags & XSHMP_CLOSED) {//Closed
        errno = EPIPE;
        return -1;
    }

    //Prepare wait structure
    await_op.request_flags = XEN_SHM_IOCTL_AWAIT_USER;
    await_op.timeout_ms = 0;

    shared_max = s->buffer + (ptrdiff_t) p->buffer_size;

    read_pos_reduced = s->buffer + (ptrdiff_t) s->read;
    read_pos_reduced = (read_pos_reduced == s->buffer)?(shared_max-1):(read_pos_reduced-1);
    write_pos = s->buffer + (ptrdiff_t) s->write ;

    //Wait for available space
    while(read_pos_reduced == write_pos) {
        if(s->reader_flags & XSHMP_CLOSED) { //Maybe it was gracefully closed
            errno = EPIPE;
            return -1;
        }

        s->writer_flags |= XSHMP_WAITING; //Notify that we are waiting
        if(ioctl(p->fd, XEN_SHM_IOCTL_AWAIT, &await_op)) {
            return -1; //Another error
        }

        //Update positions
        read_pos_reduced = s->buffer + (ptrdiff_t) s->read;
        read_pos_reduced = (read_pos_reduced == s->buffer)?(shared_max-1):(read_pos_reduced-1);
        write_pos = s->buffer + (ptrdiff_t) s->write ;
    }
    s->writer_flags &= ~XSHMP_WAITING; //Stop waiting


    user_buf = (const uint8_t*) buf;

    current_buf = user_buf;
    usr_max_buf = user_buf + (ptrdiff_t) nbytes;

    gran_max_buf = user_buf + (ptrdiff_t) XEN_SHM_PIPE_INITIAL_WAIT_CHECK_INTERVAL;
    circ_max_buf = user_buf;
    min_max_buf = user_buf;

    shared_max = s->buffer + (ptrdiff_t) p->buffer_size;

    while(write_pos != read_pos_reduced && current_buf != usr_max_buf) //We write as much as we can
    {

        /*
         * Get the minimum of different boundaries
         */
        min_max_buf = circ_max_buf;
        if(min_max_buf > usr_max_buf) {
            min_max_buf = usr_max_buf;
        }
        if(min_max_buf > gran_max_buf) {
            min_max_buf = gran_max_buf;
        }

        /*
         * Actually write
         */
        while(current_buf != min_max_buf) {
            *write_pos = *current_buf;
            ++current_buf;
            ++write_pos;
        }

        /*
         * Check boundary values
         */
        if(current_buf == gran_max_buf) { //Time to take news of the other guy
            if(s->reader_flags & XSHMP_WAITING) { //Reader is waiting
                ioctl(p->fd, XEN_SHM_IOCTL_SSIG, 0);
            }
            gran_max_buf += p->wait_check_interval;
        }

        if(write_pos == shared_max) {
            write_pos = s->buffer;
        }

        read_pos_reduced = s->buffer + (ptrdiff_t) s->read;
        read_pos_reduced = (read_pos_reduced == s->buffer)?(shared_max-1):(read_pos_reduced-1);
        s->write = (uint32_t) (write_pos - s->buffer); //Update read position in shared memory

        if(write_pos <= read_pos_reduced) {
            circ_max_buf = current_buf + (ptrdiff_t)(read_pos_reduced - write_pos); //Write up to read reduced
        } else {
            circ_max_buf = current_buf +  (ptrdiff_t)(shared_max - write_pos); //Read up to the end of the circular buffer
        }

    }

    if(s->reader_flags & XSHMP_WAITING) { //Writer is waiting
        ioctl(p->fd, XEN_SHM_IOCTL_SSIG, 0);
    }


    return (ssize_t) (current_buf - user_buf);



}


ssize_t xen_shm_pipe_write_all(xen_shm_pipe_p xpipe, const void* buf, size_t nbytes) {
    size_t written;
    ssize_t retval;
    const uint8_t* buffer;

    written = 0;
    buffer = (const uint8_t*) buf;
    while(nbytes) {
        if((retval = xen_shm_pipe_write(xpipe, buffer, nbytes ))<=0) {
            return(written!=0)?((ssize_t) written):retval;
        }
        written += (size_t) retval;
        buffer += (ptrdiff_t) retval;
        nbytes -= (size_t) retval;
    }

    return (ssize_t) written;

}


ssize_t xen_shm_pipe_read_all(xen_shm_pipe_p xpipe, void* buf, size_t nbytes) {
    size_t read;
    ssize_t retval;
    uint8_t* buffer;

    read = 0;
    buffer = (uint8_t*) buf;
    while(nbytes) {
        if((retval = xen_shm_pipe_read(xpipe, buffer, nbytes ))<=0) {
            return(read!=0)?((ssize_t) read):retval;
        }
        read += (size_t) retval;
        buffer += (ptrdiff_t) retval;
        nbytes -= (size_t) retval;
    }

    return (ssize_t) read;

}


