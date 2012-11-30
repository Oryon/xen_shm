/*
 * Xen shared memory module
 *
 * Authors: Vincent Brillault <git@lerya.net>
 *          Pierre Pfister    <oryon@darou.fr>
 *
 * This file contains the code and private headers of the
 * Xen shared memory module. See the headers file for
 * precisions about this module.
 *
 */



/*
 * Kernel module information (submitted at the end of the file)
 */
#define MOD_AUTHORS "Vincent Brillault <git@lerya.net>, Pierre Pfister <pierre.pfister@polytechnique.org>"
#define MOD_DESC "Xen Shared Memory Module"
#define MOD_LICENSE "GPL"


/*
 * Headers for file system implementation
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>

/*
 * The public header of this module
 */
#include "xen_shm.h"


/*
 * The state of the shared memory
 */
typedef uint8_t xen_shm_state_t; //The state type used

#define XEN_SHM_STATE_OPENED         //Freshly opened device, can move to offerer or receiver
#define XEN_SHM_STATE_OFFERER        //Memory is allocated. Event pipe created.
#define XEN_SHM_STATE_RECEIVER       //Memory is allocated and mapped. Pipe is connected.
#define XEN_SHM_STATE_HALF_CLOSED    //In the offerer case, we must wait the other procees to stop using the memory.


/*
 * Private function prototypes
 */

int xen_shm_init(void);
void xen_shm_cleanup(void);


/*
 * File operation functions prototype
 */
static int xen_shm_open(struct inode *, struct file *);
static int xen_shm_release(struct inode *, struct file *);
static int xen_shm_mmap(struct file *filp, struct vm_area_struct *vma);
static int xen_shm_ioctl(struct inode *, struct file *, unsigned int, unsigned long);

/*
 * Defines the device file operations
 */
struct file_operations xen_shm_file_ops {
	.ioctl = xen_shm_ioctl,
	.open = xen_shm_open,
	.release = xen_shm_release,
	.mmap = xen_shm_mmap
};

/*
 * Defines the instance information.
 * They are stored in the private data field of 'struct file' and will maybe
 * have to be kept in order to "close later" in the case of a blocking offerer close.
 */
struct xen_shm_instance_data {
	xen_shm_state_t state, //The state of this instance


	/* Pages info */
	uint8_t pages_count, //The total number of consecutive allocated pages
	uint64_t pages_phys_addr, //The physical addresses of the allocated pages

	/* Xen grant_table data */
	domid_t offerer_domid_t,  //The domain id of the offerer
	domid_t receiver_domid_t, //The domain id of the receiver

	grant_handle_t[XEN_SHM_ALLOC_ALIGNED_PAGES] grant_map_handles, //For the RECEIVER only. Contains an array with all the grant handles

	/* Event channel data */
	evtchn_port_t local_ec_port, //The allocated event port number
	evtchn_port_t dist_ec_port, //The allocated event port number

};

/*
 * The first page is used to share meta-data in a more efficient way
 */
struct xen_shm_meta_page_data {
	uint8_t offerer_closed, // The offerer signals he wants to unmap the pages. When !=0, nothing should be written anymore and the RECEIVER should close as soon as possible.
	uint8_t receiver_closed, //The receiver sets this to one just before unmapping the frames. It will not write anything on the frames after it.

	uint8_t pages_count, //The number of shared pages, with the header-page


	/*
	 * Informations about the event channel
	 */
	evtchn_port_t offerer_ec_port, //Offerer's event channel port


	/*
	 * An array containing 'pages_count' grant referances.
	 * The first needs to be sent to the receiver, but they are all written here.
	 */
	grant_ref_t[XEN_SHM_KMALLOC_MAX_ALIGNED_PAGES] grant_refs,



};


/*
 * Called when the module is loaded into the kernel
 */
int __init xen_shm_init() {

	/*
	 * Allocate a valid MAJOR number
	 */

	/*
	 * Create the node (mknod) with the allocated MAJOR number
	 */

}

/*
 * Called when the module is loaded into the kernel
 */
void __exit xen_shm_cleanup() {

	/*
	 * Need to verify if no shared memory is open ??? (maybe the kernel close them before ?)
	 */

	/*
	 * Delete the device nodes
	 */


	/*
	 * Unallocate the MAJOR number
	 */

}

/*
 * Called when a user wants to open the device
 */
static int xen_shm_open(struct inode * inode, struct file * filp) {

	/*
	 * Initialize the filp private data related to this instance.
	 */

	/*
	 * Memory is not allocated yet because the size will be specified by the user with an ioctl.
	 * So this method doesn't do so much.
	 */

}

/*
 * Called when all the processes possessing this file descriptor closed it.
 * All the allocated memory must be deallocated.
 * Statefull data must be restored.
 */
static int xen_shm_release(struct inode * inode, struct file * filp) {

	/*
	 * Warning: Remember the OFFERER grants and ungrant the pages.
	 *          The RECEIVER map and unmap the pages.
	 *          BUT the OFFERER -must not- ungrant the pages before the RECEIVER unmaped them.
	 *
	 *          Xen functions to ungrant can be used when someone still map them, but the memory (obtained with kmalloc) cannot
	 *          be freed because a new allocation of the same physical adresses would create troubles.
	 *          void gnttab_end_foreign_access(grant_ref_t ref, int readonly, unsigned long page); maybe solve the problem ? --> Not implemented yet !?!!! (see sources)
	 *			Maybe we should use the first page for information purposes ? (like using a value to know if the kmalloc can be freed)
	 */

	/*
	 * Mapped user memory needs to be unmapped
	 */

	/*
	 * Xen grant table state must be restored (unmap on receiver side and end grant on offerer side)
	 */

	/*
	 * Event channel must be closed
	 */

	/*
	 * Allocated memory must be freed
	 */

}

/*
 * Used to map the shared pages into the user address space.
 */
static int xen_shm_mmap(struct file *filp, struct vm_area_struct *vma) {

	/*
	 * Test if the memory has been allocated
	 */

	/*
	 * Map the kernel's allocated memory into the user's space
	 */

}

/*
 * Used to control an open instance.
 */
static int xen_shm_ioctl(struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg) {
    
    int retval = 0;
    
    /* Testing user's pointer */
    int err = 0; 
    
    /* Verifying value */
    if (_IOC_TYPE(cmd) != XEN_SHM_MAGIC_NUMBER) return -ENOTTY;
    //if (_IOC_NR(cmd) > XEN_SHM_IOCTL_MAXNR) return -ENOTTY; //Should be used when IOCTL MAXNR is defined
    
    /* Testing fault */
    if (_IOC_DIR(cmd) & _IOC_READ) //User wants to read, so kernel must write
        err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
    else if (_IOC_DIR(cmd) & _IOC_WRITE) //User wants to write, so kernel must read
        err = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
    
    if (err) return -EFAULT;
    
    
   switch (cmd) {
       case XEN_SHM_IOCTL_INIT_OFFERER:
           /*
            * Used to make the state go from OPENED to OFFERER.
            * 
            * Note the first allocated page is used to transfer the event channel port.
            * When multiple pages are alocated, the first page is also used to transfer
            * the grant_ref_t array.
            */
           
           
           break;
       case XEN_SHM_IOCTL_INIT_RECEIVER:
           /*
            * Used to make the state go from OPENED to RECEIVER.
            * 
            */
           
           
           break;
       case XEN_SHM_IOCTL_WAIT:
           /*
            * Immediatly sends a signal through the signal channel
            */
           
           
           break;
       case XEN_SHM_IOCTL_SSIG:
           /*
            * Waits until a signal is received through the signal channel
            */
           
           
           break;
       case XEN_SHM_IOCTL_GET_DOMID:
           /*
            * Writes the domain id into the structure
            */
           
           
           break;
       default:
           return -ENOTTY;
           break;

   }

    
	


}


/*
 * Module functions
 */
module_init(xen_shm_init);
module_exit(xen_shm_cleanup);

/*
 * Module informations
 */

MODULE_LICENSE(MOD_LICENSE);
MODULE_AUTHOR(MOD_AUTHORS);
MODULE_DESCRIPTION(MOD_DESC);
