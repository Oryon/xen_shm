/*
 * Xen shared memory pipe headers
 *
 * Authors: Vincent Brillault <git@lerya.net>
 *          Pierre Pfister    <oryon@darou.fr>
 *
 * This file contains the headers of the Xen shared memory pipe
 * tool.
 *
 * It provides a very fast way to transfer data from
 * a virtual machine process to another one (running on
 * a possibly different virtual machine), when they run
 * on the same XEN Hypervisor.
 *
 */

#ifndef __XEN_SHM_PIPE_H__
#define __XEN_SHM_PIPE_H__

#include <inttypes.h>

#define XSHMP_STATS

#ifdef XSHMP_STATS
struct xen_shm_pipe_stats {
    uint64_t ioctl_count_await;
    uint64_t ioctl_count_ssig;
    uint64_t read_count;
    uint64_t write_count;
};
#endif



/*
 * Everything is private within this structure. Therefore, the user cannot manipulate it.
 */
typedef void* xen_shm_pipe_p;

/*
 * The pipe mode on this side
 */
enum xen_shm_pipe_mod {
    xen_shm_pipe_mod_write,
    xen_shm_pipe_mod_read
};


/*
 * The direction convention between writer/reader offerer/receiver
 */
enum  xen_shm_pipe_conv {
    xen_shm_pipe_conv_writer_offers,
    xen_shm_pipe_conv_reader_offers
};




/*
 * Init a pipe. Return 0 if ok. A negative value otherwise.
 * Warning: The Offerer MUST initialize first
 */
int xen_shm_pipe_init(xen_shm_pipe_p * pipe,  /* A returned pointer to a pipe */
                      enum xen_shm_pipe_mod mod,   /* The pipe mod (writer or reader) */
                      enum xen_shm_pipe_conv conv  /* The convention of the pipe */
                      );

/*
 * Receiver's side
 */

/* 1. Get receiver's domid to send it to the offerer */
int xen_shm_pipe_getdomid(xen_shm_pipe_p pipe, uint32_t* receiver_domid);

/* 2. Send the domid to the offerer */

/* 3. Receive offerer's domid, grant ref and page_count */

/* 4. Connects with the offerer */
int xen_shm_pipe_connect(xen_shm_pipe_p pipe, uint8_t page_count, uint32_t offerer_domid, uint32_t grant_ref);


/*
 * Offerer's side
 */

/* 1. Receive receiver's domid */

/* 2. Sets it, start sharing and get offerer's domid and grant ref */
int xen_shm_pipe_offers(xen_shm_pipe_p pipe, uint8_t page_count, uint32_t receiver_domid, uint32_t* offerer_domid, uint32_t* grant_ref);

/* 3. Sends offerer domid, grant ref and page_count to the receiver */

/* 4. Waits for the receiver to connect */
int xen_shm_pipe_wait(xen_shm_pipe_p pipe, unsigned long timeout_ms);



/*
 * Writes into the pipe. Returns the number of written bytes or -1 and errno is set.
 */
ssize_t xen_shm_pipe_write(xen_shm_pipe_p pipe, const void* buf, size_t nbytes);

/*
 * Does as write but retry and block until no more bytes can be written or all the nbytes were actually written.
 * Warning: When the pipe is closing, the number of written bytes can take any value between 1 and nbytes.
 */
ssize_t xen_shm_pipe_write_all(xen_shm_pipe_p pipe, const void* buf, size_t nbytes);

/*
 * Read in the pipe. Returns the number of read bytes, 0 if EOF, or -1 and errno is set.
 */
ssize_t xen_shm_pipe_read(xen_shm_pipe_p pipe, void* buf, size_t nbytes);

/*
 * Does as read but retry and block until no more bytes can be read.
 * Warning: When the pipe is closing, the number of read bytes can take any value between 1 and nbytes.
 */
ssize_t xen_shm_pipe_read_all(xen_shm_pipe_p pipe, void* buf, size_t nbytes);

/*
 * All pipes must be freed when they are not used anymore.
 */
void xen_shm_pipe_free(xen_shm_pipe_p);


#ifdef XSHMP_STATS
/*
 * Get the statistics about the pipe
 */
struct xen_shm_pipe_stats xen_shm_pipe_get_stats(xen_shm_pipe_p pipe);
#endif

#endif






