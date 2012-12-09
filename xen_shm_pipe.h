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


/*
 * Enables statistics to be gathered during the transfert
 */
#define XSHMP_STATS



#ifdef XSHMP_STATS

/*
 * The statistics that are gathered
 */
struct xen_shm_pipe_stats {
    uint64_t ioctl_count_await;
    uint64_t ioctl_count_epipe_prone;
    uint64_t ioctl_count_ssig;
    uint64_t read_count;
    uint64_t write_count;
    uint8_t waiting;
};
#endif



/*
 * Everything is private within this structure. Therefore, the user cannot manipulate it.
 */
typedef void* xen_shm_pipe_p;

/*
 * The pipe is unidirectional. One can either write or read.
 */
enum xen_shm_pipe_mod {
    xen_shm_pipe_mod_write,
    xen_shm_pipe_mod_read
};


/*
 * The direction convention between writer/reader offerer/receiver.
 * It *must* be the same for both ends.
 */
enum  xen_shm_pipe_conv {
    xen_shm_pipe_conv_writer_offers,
    xen_shm_pipe_conv_reader_offers
};




/*
 * Init a pipe.
 * On succes, returns 0. On error, -1 is returned, and errno is set appropriately.
 */
int xen_shm_pipe_init(xen_shm_pipe_p * pipe,  /* A returned pointer to a pipe */
                      enum xen_shm_pipe_mod mod,   /* The pipe mod (writer or reader) */
                      enum xen_shm_pipe_conv conv  /* The convention of the pipe */
                      );

/*
 * Receiver's side steps
 * Those functions all returns 0 on success and -1 on error and errno is set appropriately.
 */

/* 1. Get receiver's domid to send it to the offerer */
int xen_shm_pipe_getdomid(xen_shm_pipe_p pipe, uint32_t* receiver_domid);

/* 2. Send the domid to the offerer */

/* 3. Receive offerer's domid, grant ref and page_count */

/* 4. Connects with the offerer */
int xen_shm_pipe_connect(xen_shm_pipe_p pipe, uint8_t page_count, uint32_t offerer_domid, uint32_t grant_ref);


/*
 * Offerer's side steps
 * Those functions all returns 0 on success and -1 on error and errno is set appropriately.
 */

/* 1. Receive receiver's domid */

/* 2. Sets it, start sharing and get offerer's domid and grant ref */
int xen_shm_pipe_offers(xen_shm_pipe_p pipe, uint8_t page_count, uint32_t receiver_domid, uint32_t* offerer_domid, uint32_t* grant_ref);

/* 3. Sends offerer domid, grant ref and page_count to the receiver */

/* 4. Waits for the receiver to connect */
int xen_shm_pipe_wait(xen_shm_pipe_p pipe, unsigned long timeout_ms);



/*
 * Writes into the pipe. Returns the number of written bytes or -1 and errno is set approprietely.
 * Blocks until at least one byte is written or an error occurs.
 */
ssize_t xen_shm_pipe_write(xen_shm_pipe_p pipe, const void* buf, size_t nbytes);

/*
 * Behave as write but retry and blocks until no more bytes can be written or nbytes were written.
 * If an error occurs while some bytes were already written, the number of bytes is returned.
 * If an error occurs while no byte were written, -1 is returned and errno is set approprietely.
 */
ssize_t xen_shm_pipe_write_all(xen_shm_pipe_p pipe, const void* buf, size_t nbytes);

/*
 * Read in the pipe. Returns the number of read bytes, 0 if EOF, or -1 and errno is set.
 * Blocks until at least one byte is read or an error occurs.
 */
ssize_t xen_shm_pipe_read(xen_shm_pipe_p pipe, void* buf, size_t nbytes);

/*
 * Behave as read but retry and blocks until no more bytes can be read of nbytes were read.
 * If an error occurs, or the pipe is closed, while some bytes were already written, the number of bytes is returned.
 * If an error occurs while no byte were written, -1 is returned and errno is set approprietely.
 * If the pipe is closed while no byte were written, 0 is returned.
 */
ssize_t xen_shm_pipe_read_all(xen_shm_pipe_p pipe, void* buf, size_t nbytes);

/*
 * All pipes must be freed when they are not used anymore.
 * It closes the pipe and free the memory.
 */
void xen_shm_pipe_free(xen_shm_pipe_p);


#ifdef XSHMP_STATS
/*
 * Get the statistics about the pipe
 */
struct xen_shm_pipe_stats xen_shm_pipe_get_stats(xen_shm_pipe_p pipe);
#endif

#endif






