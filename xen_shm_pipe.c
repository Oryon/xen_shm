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


/* Private data about the pipe */
struct xen_shm_pipe_priv {
    int fd;
    xen_shm_pipe_mod mod;
    xen_shm_pipe_mod conv;
    uint8_t page_count;


};

/* Structure of the shared area */
struct xen_shm_pipe_shared {


};

