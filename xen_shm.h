/*
 * Xen shared memory module headers
 *
 * Authors: Vincent Brillault <git@lerya.net>
 *          Pierre Pfister    <oryon@darou.fr>
 *
 * This file contains the public headers of the Xen
 * shared memory module. It provides the user-space all
 * the needed structures and values to use to xen_shm
 * device.
 *
 * This module gives the users of virtual machine over
 * a Xen Hypervisor a way to share pages of memory with
 * some other program on a possibly different virtual
 * machine.
 * It also provides waiting and notifying mechanisms
 * between the two processes.
 *
 */

#include <linux/ioctl.h>
#include <xen/gnttab.h>



/*
 * General configuration
 */


/* The maximum number of aligned pages that can be provided */
#define XEN_SHM_ALLOC_ALIGNED_PAGES 16

/* The maximum number of pages the shared-memory module can provide to the user */
#define XEN_SHM_MAX_SHARED_PAGES (XEN_SHM_ALLOC_ALIGNED_PAGES - 1)

/* The device major number - 0 for automatic allocation */
#define XEN_SHM_MAJOR_NUMBER 0



/*
 * IOCTL's command numbers and structures
 * A Magic number is defined and used. Using those Macros reduces the probability 
 * of collisions and provides information to check the user's argument pointer
 */

#define XEN_SHM_MAGIC_NUMBER 83 // 8 bit int

/* 
 * Init the shared memory as the offerer domain
 */
#define XEN_SHM_IOCTL_INIT_OFFERER    _IOWR(XEN_SHM_MAGIC_NUMBER, 1, struct xen_shm_ioctlarg_offerer )
struct xen_shm_ioctlarg_offerer {
    /* In arguments */
    uint8_t pages_count,
    domid_t dist_domid,
    
    /* Out arguments */
    grant_ref_t grant,
};

/* 
 * Init the shared memory as the receiver domain
 */
#define XEN_SHM_IOCTL_INIT_RECEIVER   _IOWR(XEN_SHM_MAGIC_NUMBER, 2, struct xen_shm_ioctlarg_receiver )
struct xen_shm_ioctlarg_receiver {
    /* In arguments */
    uint8_t pages_count,
    domid_t dist_domid,
    grant_ref_t grant,
    
    /* Out arguments */
    
};

/* 
 * Blocks until a signal is received through the event channel
 * Argument is ignored
 */
#define XEN_SHM_IOCTL_WAIT            _IO(XEN_SHM_MAGIC_NUMBER, 3)


/* 
 * Sends a signal through the event channel
 * Argument is ignored
 */
#define XEN_SHM_IOCTL_SSIG            _IO(XEN_SHM_MAGIC_NUMBER, 4)


/* 
 * Get the machine's domain id
 */
#define XEN_SHM_IOCTL_GET_DOMID       _IOR(XEN_SHM_MAGIC_NUMBER, 5, struct xen_shm_ioctlarg_getdomid )
struct xen_shm_ioctlarg_getdomid {
    /* In arguments */
    
    /* Out arguments */
    domid_t local_domid,
    
};


