/*
 * Xen shared memory pipe headers
 * 
 * Authors: Vincent Brillault <git@lerya.net> 
 *          Pierre Pfister    <pierre.pfister@polytechnique.org>
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


/*
 * Everything is private within this structure. Therefore, the user cannot manipulate it.
 */
typedef void* xen_shm_pipe_p;

/* 
 * The pipe mode on this side
 */
typedef uint8_t xen_shm_pipe_mod;

#define XEN_SHM_PIPE_MOD_READ 0
#define XEN_SHM_PIPE_MOD_WRITE 1

/* 
 * The direction convention between writer/reader offerer/receiver
 */
typedef uint8_t xen_shm_pipe_dir_conv;

#define XEN_SHM_PIPE_DIR_WRITER_OFFERER 0
#define XEN_SHM_PIPE_DIR_READER_OFFERER 1



/*
 * This information MUST be the same on both side when initializing the pipe.
 * The offerer MUST initialize its side first.
 */
struct xen_shm_sync {
	size_t shm_size;
	uint32_t grant_ref;
	xen_shm_pipe_dir_conv direction;
};



/*
 * Init a pipe. Return 0 if ok. A negative value otherwise. 
 * Warning: The Offerer MUST initialize first
 */
int xen_shm_init_pipe(xen_shm_pipe_p *, /* A pointer to a pipe */
						uint32_t domain_id, /* The other domain id */
						struct xen_shm_sync, /* The sync variables */
						xen_shm_pipe_mod /* The pipe mod (writer or reader) */
					  );


/*
 * All pipes must be freed when they are not used anymore.
 */
int xen_shm_close_pipe(xen_shm_pipe_p);



size_t xen_shm_write(xen_shm_pipe_p pipe, const void* buf, size_t nbytes);


size_t xen_shm_read(xen_shm_pipe_p pipe, void* buf, size_t nbytes);




