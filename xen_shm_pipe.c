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
#define XSHMP_OPENED   0x00000001u
#define XSHMP_CLOSED   0x00000002u
#define XSHMP_WAITING  0x00000004u
#define XSHMP_SLEEPING 0x00000008u





/* Private data about the pipe */
struct xen_shm_pipe_priv {
    int fd;
    enum xen_shm_pipe_mod mod;
    enum xen_shm_pipe_conv conv;

    struct xen_shm_pipe_shared* shared;

    size_t buffer_size;
    ptrdiff_t wait_check_interval;
    struct xen_shm_ioctlarg_await await_op;
    int saw_epipe;


#ifdef XSHMP_STATS
    struct xen_shm_pipe_stats stats;
#endif

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
int __xen_shm_pipe_send_signal(struct xen_shm_pipe_priv* p);
int __xen_shm_pipe_wait_signal(struct xen_shm_pipe_priv* p);
int __xen_shm_pipe_wait_writer(struct xen_shm_pipe_priv* p);
int __xen_shm_pipe_wait_reader(struct xen_shm_pipe_priv* p);
size_t __xen_shm_pipe_read_avail(struct xen_shm_pipe_priv* p, void* buf, size_t nbytes);
size_t __xen_shm_pipe_write_avail(struct xen_shm_pipe_priv* p, const void* buf, size_t nbytes);


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
    p->await_op.request_flags = XEN_SHM_IOCTL_AWAIT_USER;
    p->await_op.timeout_ms = 0;
    p->saw_epipe = 0;
    *xpipe = p;

#ifdef XSHMP_STATS
    p->stats.ioctl_count_await = 0;
    p->stats.ioctl_count_ssig = 0;
    p->stats.read_count = 0;
    p->stats.write_count = 0;
    p->stats.waiting = 0;
#endif

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

int
__xen_shm_pipe_send_signal(struct xen_shm_pipe_priv* p) {
#ifdef XSHMP_STATS
            p->stats.ioctl_count_ssig++;
#endif
            return ioctl(p->fd, XEN_SHM_IOCTL_SSIG, 0);
}

int
__xen_shm_pipe_wait_signal(struct xen_shm_pipe_priv* p) {
#ifdef XSHMP_STATS
            p->stats.ioctl_count_await++;
#endif
            return ioctl(p->fd, XEN_SHM_IOCTL_AWAIT, &p->await_op);
}

/* Waits for available bytes to read. Return -1 if error. 0 if end of file. 1 if bytes available. */
int
__xen_shm_pipe_wait_reader(struct xen_shm_pipe_priv* p) {
    struct xen_shm_pipe_shared* s;
    volatile struct xen_shm_pipe_shared* sv;

    uint32_t writer_flags;
    uint32_t read;
    int retval;
    int unset_wait;

    s = p->shared;
    sv = p->shared;

    unset_wait = 0;
    read = s->read;
    while(read == sv->write) {

        writer_flags = sv->writer_flags;

        s->reader_flags |= XSHMP_WAITING; //Say we are waiting
        unset_wait = 1;

        if(read != sv->write) { //Check nothing changed
            break;
        }

        if(writer_flags & XSHMP_CLOSED) { //File was closed and no bytes remaining
            return 0;
        }

        if(p->saw_epipe) { //File is not closed but we saw a EPIPE. It's an error.
            errno = EPIPE;
            return -1;
        }

        if(writer_flags & XSHMP_SLEEPING) { //Other is sleeping, must send a signal
            __xen_shm_pipe_send_signal(p);
            continue;
        }

        if(writer_flags & XSHMP_WAITING) { //Other is waiting, must do an active wait
            continue;
        }

        s->reader_flags |= XSHMP_SLEEPING; //Say we are sleeping
        retval = __xen_shm_pipe_wait_signal(p);
        s->reader_flags &= ~XSHMP_SLEEPING; //Wake up !
        if(retval == -1) {
            if(errno == EPIPE) {
                p->saw_epipe = 1;
                continue;
            } else {
                s->writer_flags &= ~XSHMP_WAITING;
                return -1;
            }
        }

    }

    if(unset_wait) {
        s->reader_flags &= ~XSHMP_WAITING;
    }

    return 1;
}


/* Waits for available bytes to read. Return -1 if error. 0 if end of file. 1 if bytes available. */
int
__xen_shm_pipe_wait_writer(struct xen_shm_pipe_priv* p) {
    struct xen_shm_pipe_shared* s;
    volatile struct xen_shm_pipe_shared* sv;

    uint32_t reader_flags;
    uint32_t write;
    int retval;
    int unset_wait;

    s = p->shared;
    sv = p->shared;

    unset_wait = 0;

    if(sv->reader_flags & XSHMP_CLOSED) { //File was closed
        return -1;
    }

    write = s->write + 1;
    if(write == p->buffer_size) {
        write = 0;
    }

    while(write == sv->read) {

        reader_flags = sv->reader_flags;

        s->writer_flags |= XSHMP_WAITING; //Say we are waiting
        unset_wait = 1;

        if(write != sv->read) { //Check nothing changed
            break;
        }

        if(reader_flags & XSHMP_CLOSED) { //File was closed
            return -1;
        }

        if(reader_flags & XSHMP_SLEEPING) { //Other is sleeping, must send a signal
            __xen_shm_pipe_send_signal(p);
            continue;
        }

        if(reader_flags & XSHMP_WAITING) { //Other is waiting, must do an active wait
            continue;
        }

        s->writer_flags |= XSHMP_SLEEPING; //Say we are sleeping
        retval = __xen_shm_pipe_wait_signal(p);
        s->writer_flags &= ~XSHMP_SLEEPING; //Wake up !
        if(retval == -1) {
            s->writer_flags &= ~XSHMP_WAITING;
            return -1;
        }

    }

    if(unset_wait) {
        s->writer_flags &= ~XSHMP_WAITING;
    }

    return 1;
}

size_t
__xen_shm_pipe_read_avail(struct xen_shm_pipe_priv* p, void* buf, size_t nbytes) {
    struct xen_shm_pipe_shared* s;
    volatile struct xen_shm_pipe_shared* sv;

    uint8_t* user_buf; //A cast of the given user buffer

    uint8_t* read_pos; //Read pointer in circular buffer
    uint64_t* read_pos64;//64b aligned
    uint8_t* write_pos;//Write pointer in circ buff
    uint8_t* shared_max; //Out of bound pointer in circ buffer

    uint8_t* current_buf;//Current write pos
    uint64_t* current_buf64;//64b aligned

    uint8_t* usr_max_buf;//Out of bound given by the user
    uint8_t* gran_max_buf;//Out of bound to check if the writer is waiting
    uint8_t* circ_max_buf;//Out of bound given by the circ buffer

    uint8_t* min_max_buf;//Min value of the 3 different out of bound
    uint64_t* min_max_buf64;//64b aligned

    s = p->shared;
    sv = p->shared;

    shared_max = s->buffer + (ptrdiff_t) p->buffer_size;
    read_pos = s->buffer + (ptrdiff_t) s->read ;
    write_pos = s->buffer + (ptrdiff_t) sv->write ;

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
        //Tests 64b alignement
        if( (((unsigned long) current_buf) & ((unsigned long) 0x7u)) == (((unsigned long)read_pos) & ((unsigned long) 0x7u))
                && min_max_buf - current_buf > 24) {
            //Align optimized
            while( ((unsigned long) current_buf) & ((unsigned long) 0x7u)) { //Slow copy to align with 64b pointers
                *current_buf = *read_pos;
                ++current_buf;
                ++read_pos;
            }

            min_max_buf64 = (uint64_t*)( ((unsigned long)min_max_buf) & ~((unsigned long) 0x7u)  );
            current_buf64 = (uint64_t*)(current_buf);
            read_pos64 = (uint64_t*)(read_pos);
            while(current_buf64 != min_max_buf64) { //Fast copy
                *current_buf64 = *read_pos64;
                ++current_buf64;
                ++read_pos64;
            }
            current_buf = (uint8_t*)(current_buf64);
            read_pos = (uint8_t*)(read_pos64);

            //Ending the copy
            while(current_buf != min_max_buf) {
                *current_buf = *read_pos;
                ++current_buf;
                ++read_pos;
            }

        } else {
            //Slow speed
            while(current_buf != min_max_buf) {
                *current_buf = *read_pos;
                ++current_buf;
                ++read_pos;
            }
        }


        /*
         * Check boundary values
         */
        if(current_buf == gran_max_buf) { //Time to take news of the other guy
            if(sv->writer_flags & XSHMP_SLEEPING) { //Writer is waiting
                __xen_shm_pipe_send_signal(p);
            }
            gran_max_buf += p->wait_check_interval;
        }

        if(read_pos == shared_max) { //Time to check if the read pointer must be rewind
            read_pos = s->buffer;
        }

        write_pos = s->buffer + (ptrdiff_t) sv->write; //Updates write_pos value (it could have changed)
        s->read = (uint32_t) (read_pos - s->buffer); //Update read position in shared memory

        if(read_pos <= write_pos) { //Updates the circ buffer threshold
            circ_max_buf = current_buf + (ptrdiff_t)(write_pos - read_pos);
        } else {
            circ_max_buf = current_buf + (ptrdiff_t)(shared_max - read_pos);
        }


    }

    if(sv->writer_flags & XSHMP_SLEEPING) { //Writer is waiting
        __xen_shm_pipe_send_signal(p);
    }


    return (size_t) (current_buf - user_buf);

}

size_t
__xen_shm_pipe_write_avail(struct xen_shm_pipe_priv* p, const void* buf, size_t nbytes) {
    struct xen_shm_pipe_shared* s;
    volatile struct xen_shm_pipe_shared* sv;

    uint8_t* read_pos_reduced; //Read pointer - 1 in circular buffer
    uint8_t* write_pos;//Write pointer in circ buff
    uint64_t* write_pos64;//64b aligned
    uint8_t* shared_max; //Out of bound pointer in circ buffer

    const uint8_t* user_buf; //A cast of the given user buffer
    const uint8_t* current_buf;//Current write pos
    const uint64_t* current_buf64;//64b aligned

    const uint8_t* usr_max_buf;//Out of bound given by the user
    const uint8_t* gran_max_buf;//Out of bound to check if the reader is waiting
    const uint8_t* circ_max_buf;//Out of bound given by the circ buffer

    const uint8_t* min_max_buf;//Min value of the 3 different out of bound
    const uint64_t* min_max_buf64;//64b aligned

    s = p->shared;
    sv = p->shared;

    shared_max = s->buffer + (ptrdiff_t) p->buffer_size;

    read_pos_reduced = s->buffer + (ptrdiff_t) sv->read;
    read_pos_reduced = (read_pos_reduced == s->buffer)?(shared_max-1):(read_pos_reduced-1);
    write_pos = s->buffer + (ptrdiff_t) s->write ;

    user_buf = (const uint8_t*) buf;

    current_buf = user_buf;
    usr_max_buf = user_buf + (ptrdiff_t) nbytes;

    gran_max_buf = user_buf + (ptrdiff_t) XEN_SHM_PIPE_INITIAL_WAIT_CHECK_INTERVAL;
    circ_max_buf = user_buf;
    min_max_buf = user_buf;

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

        //Tests 64b alignement
        if(  (((unsigned long) current_buf) & ((unsigned long) 0x7u)) == (((unsigned long) write_pos) & ((unsigned long) 0x7u))
                && min_max_buf - current_buf > 24  ) {
            //Align optimized
            while( ((unsigned long) current_buf) & ((unsigned long) 0x7u)) { //Slow copy to align with 64b pointers
                *write_pos = *current_buf;
                ++current_buf;
                ++write_pos;
            }

            min_max_buf64 = (uint64_t*)( ((unsigned long) min_max_buf) & ~((unsigned long) 0x7u)); //Previous aligned
            current_buf64 = (const uint64_t*)(current_buf);
            write_pos64 = (uint64_t*)(write_pos);
            while(current_buf64 != min_max_buf64) { //Fast copy
                *write_pos64 = *current_buf64;
                ++current_buf64;
                ++write_pos64;
            }
            current_buf = (const uint8_t*)(current_buf64);
            write_pos = (uint8_t*)(write_pos64);

            //Ending the copy
            while(current_buf != min_max_buf) {
                *write_pos = *current_buf;
                ++current_buf;
                ++write_pos;
            }

        } else {
            //Slow speed
            while(current_buf != min_max_buf) {
                *write_pos = *current_buf;
                ++current_buf;
                ++write_pos;
            }
        }

        /*
         * Check boundary values
         */
        if(current_buf == gran_max_buf) { //Time to take news of the other guy
            if(sv->reader_flags & XSHMP_SLEEPING) { //Reader is waiting
                __xen_shm_pipe_send_signal(p);
            }
            gran_max_buf += p->wait_check_interval;
        }

        if(write_pos == shared_max) {
            write_pos = s->buffer;
        }

        read_pos_reduced = s->buffer + (ptrdiff_t) sv->read;
        read_pos_reduced = (read_pos_reduced == s->buffer)?(shared_max-1):(read_pos_reduced-1);
        s->write = (uint32_t) (write_pos - s->buffer); //Update read position in shared memory

        if(write_pos <= read_pos_reduced) {
            circ_max_buf = current_buf + (ptrdiff_t)(read_pos_reduced - write_pos); //Write up to read reduced
        } else {
            circ_max_buf = current_buf +  (ptrdiff_t)(shared_max - write_pos); //Write up to the end of the circular buffer
        }

    }

    if(sv->reader_flags & XSHMP_SLEEPING) { //Reader is waiting
        __xen_shm_pipe_send_signal(p);
    }


    return (size_t) (current_buf - user_buf);
}

ssize_t
xen_shm_pipe_read(xen_shm_pipe_p xpipe, void* buf, size_t nbytes)
{
    struct xen_shm_pipe_priv* p;
    int wait_ret;

    p = xpipe;

#ifdef XSHMP_STATS
    p->stats.read_count++;
#endif

    if(p->mod == xen_shm_pipe_mod_write) { //Not reader
        errno = EMEDIUMTYPE;
        return -1;
    }

    if(p->shared == NULL) { //Not initialized
        errno = EMEDIUMTYPE;
        return -1;
    }

    if(p->shared->reader_flags & XSHMP_CLOSED) {//Closed
        return 0;
    }

    wait_ret = __xen_shm_pipe_wait_reader(p);
    if(wait_ret <= 0) {
        return (ssize_t) wait_ret;
    }

    return (ssize_t) __xen_shm_pipe_read_avail(p, buf, nbytes);
}




ssize_t
xen_shm_pipe_write(xen_shm_pipe_p xpipe, const void* buf, size_t nbytes) {
    struct xen_shm_pipe_priv* p;
    int wait_ret;

    p = xpipe;

#ifdef XSHMP_STATS
    p->stats.write_count++;
#endif
    if(p->mod == xen_shm_pipe_mod_read) { //Not reader
        errno = EMEDIUMTYPE;
        return -1;
    }

    if(p->shared == NULL) { //Not initialized
        errno = EMEDIUMTYPE;
        return -1;
    }

    if(p->shared->writer_flags & XSHMP_CLOSED) {//Closed
        errno = EPIPE;
        return -1;
    }

    wait_ret = __xen_shm_pipe_wait_writer(p);
    if(wait_ret <= 0) {
        return (ssize_t) wait_ret;
    }

    return (ssize_t) __xen_shm_pipe_write_avail(p, buf, nbytes);

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

#ifdef XSHMP_STATS
struct xen_shm_pipe_stats xen_shm_pipe_get_stats(xen_shm_pipe_p xpipe) {
    struct xen_shm_pipe_priv* p;
    p = xpipe;
    return p->stats;
}
#endif
