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
#define MOD_AUTHORS "Vincent Brillault <git@lerya.net>, Pierre Pfister <oryon@darou.fr>"
#define MOD_DESC "Xen Shared Memory Module"
#define MOD_LICENSE "GPL"


/*
 * Headers for file system implementation
 */
#include <asm/xen/hypercall.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <xen/interface/xen.h>
#include <xen/interface/event_channel.h>
#include <asm/page.h>

/*
 * Fix USHRT_MAX declaration on strange linux
 */
#ifndef USHRT_MAX
# define USHRT_MAX  ((u16)(~0U))
#endif

/*
 * The public header of this module
 */
#include "xen_shm.h"


/*
 * The state of the shared memory
 */
typedef uint8_t xen_shm_state_t; //The state type used

#define XEN_SHM_STATE_OPENED        0x01 //Freshly opened device, can move to offerer or receiver
#define XEN_SHM_STATE_OFFERER       0x02 //Memory is allocated. Event pipe created.
#define XEN_SHM_STATE_RECEIVER      0x03 //Memory is allocated and mapped. Pipe is connected.
#define XEN_SHM_STATE_HALF_CLOSED   0x04 //In the offerer case, we must wait the other procees to stop using the memory.


/*
 * Private function prototypes
 */

int xen_shm_init(void);
void xen_shm_cleanup(void);

/*
 * Global values
 */

static domid_t xen_shm_domid = 0; //Must only be used in open Use instance_data to get it otherwise. 
static int xen_shm_major_number = XEN_SHM_MAJOR_NUMBER;
static int xen_shm_minor_number = 0;
static dev_t xen_shm_device = 0;

/*
 * Module parameters
 * As we don't want to define the checkers for domid_t, let's say it's a ushort
 */
module_param(xen_shm_domid, ushort, S_IRUSR | S_IRGRP);
MODULE_PARM_DESC(xen_shm_domid, "Local domain id");


/*
 * File operation functions prototype
 */
static int xen_shm_open(struct inode *, struct file *);
static int xen_shm_release(struct inode *, struct file *);
static int xen_shm_mmap(struct file *filp, struct vm_area_struct *vma);
static long xen_shm_ioctl(struct file *, unsigned int, unsigned long);

/*
 * Defines the device file operations
 */
const struct file_operations xen_shm_file_ops = {
	.unlocked_ioctl = xen_shm_ioctl,
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
	xen_shm_state_t state; //The state of this instance

    
	/* Pages info */
	uint8_t pages_count; //The total number of consecutive allocated pages (with the header page)
    unsigned int alloc_order;  //Saved value of 'order'. Is used when freeing the pages
	unsigned long shared_memory; //The kernel addresses of the allocated pages (can also be void*)

	/* Xen grant_table data */
	domid_t local_domid;    //The local domain id
	domid_t distant_domid; //The distant domain id

	grant_handle_t grant_map_handles[XEN_SHM_ALLOC_ALIGNED_PAGES]; //For the RECEIVER only. Contains an array with all the grant handles

	/* Event channel data */
	evtchn_port_t local_ec_port; //The allocated event port number
	evtchn_port_t dist_ec_port; //The allocated event port number

};

/*
 * The first page is used to share meta-data in a more efficient way
 */
struct xen_shm_meta_page_data {
	uint8_t offerer_closed; // The offerer signals he wants to unmap the pages. When !=0, nothing should be written anymore and the RECEIVER should close as soon as possible.
	uint8_t receiver_closed; //The receiver sets this to one just after unmapping the frames.
    
	uint8_t pages_count; //The number of shared pages, with the header-page. The offerer writes it and the receiver must check it agrees with what he wants


	/*
	 * Informations about the event channel
	 */
	evtchn_port_t offerer_ec_port; //Offerer's event channel port


	/*
	 * An array containing 'pages_count' grant referances.
	 * The first needs to be sent to the receiver, but they are all written here.
	 */
	grant_ref_t grant_refs[XEN_SHM_ALLOC_ALIGNED_PAGES];



};

static int get_domid_hack(void) {
    int retval;
    /* Structs */
    struct evtchn_alloc_unbound alloc_unbound = {
        .dom = DOMID_SELF,
        .remote_dom = DOMID_SELF
    };
    struct evtchn_status status;
    struct evtchn_close close;

    /* Open a unbound channel */
    retval = HYPERVISOR_event_channel_op(EVTCHNOP_alloc_unbound, &alloc_unbound);
    if (retval != 0) {
        /* Something went wrong */
        printk(KERN_WARNING "xen_shm: Unable to open an unbound channel (%i)\n", retval);
        return -EIO;
    }

    /* Get the channel status */
    /* Update request */
    status.dom = DOMID_SELF;
    status.port = alloc_unbound.port;

    /* Hypervisor call (check return value later)*/
    retval = HYPERVISOR_event_channel_op(EVTCHNOP_status, &status);

    /* Close the channel */
    /* Update request */
    close.port = alloc_unbound.port;

    /* If it doesn't close, we are doomed, but that's life */
    (void) HYPERVISOR_event_channel_op(EVTCHNOP_close, &close);

    /* Verify that the status returned correctly */
    if (retval != 0) {
        /* Something went wrong */
        printk(KERN_WARNING "xen_shm: Unable to get the status of the unbound channel (%i)\n", retval);
        return -EIO;
    }

    /* check the status */
    if (status.status != EVTCHNSTAT_unbound) {
        /* Should have been unbound, let's die */
        printk(KERN_WARNING "xen_shm: Bad status of the unbound channel (%i)\n", status.status);
        return -EAGAIN;
    }

    /* Return the domid */
    return (int) status.u.unbound.dom;
}

/*
 * Called when the module is loaded into the kernel
 */
int __init
xen_shm_init()
{
    int res;
    
    /*
     * Check page size with respect to sizeof(struct xen_shm_meta_page_data)
     */
    if (sizeof(struct xen_shm_meta_page_data) > PAGE_SIZE) {
        printk(KERN_WARNING "xen_shm: xen_shm_meta_page_data is larger than a single page - So it can't work ! ");
        return -1; //TODO: Retourner une valeur correcte
    }
    
	/*
	 * Allocate a valid MAJOR number
	 */
    if (xen_shm_major_number) { //If the major number is defined
        xen_shm_device = MKDEV(xen_shm_major_number, xen_shm_minor_number);
        res = register_chrdev_region(xen_shm_device, 1, "xen_shm");
    } else { //If it is not defined, dynamic allocation
        res = alloc_chrdev_region(&xen_shm_device,  xen_shm_minor_number, 1, "xen_shm" );
        xen_shm_major_number = MAJOR(xen_shm_device);
    }

    if (res < 0) {
        printk(KERN_WARNING "xen_shm: can't get major %d\n", xen_shm_major_number);
        return res;
    }

    if (xen_shm_domid == 0) {
         /* Let's try to get it by ouselves */
         res = get_domid_hack();
         if (res < 0) {
             printk(KERN_WARNING "xen_shm: can't obtain local domid, try to set it by yourself (%i)\n", res);
             return res;
         }
         if ((unsigned int)res > USHRT_MAX) {
             printk(KERN_WARNING "xen_shm: Obtained a domid which isn't a ushort, should not be possible (%i)\n", res);
             return res;
         }
         xen_shm_domid = (domid_t) res;
         printk(KERN_INFO "xen_shm: Obtained domid by myself: %i\n", res);
    }

    return 0;
}

/*
 * Called when the module is loaded into the kernel
 */
void __exit
xen_shm_cleanup()
{
	/*
	 * Needs to verify if no shared memory is open ??? (maybe the kernel close them before ?)
	 */


	/*
	 * Unallocate the MAJOR number
	 */
    unregister_chrdev_region(xen_shm_device, 1);

}

/*
 * Called when a user wants to open the device
 */
static int
xen_shm_open(struct inode * inode, struct file * filp)
{

	/*
	 * Initialize the filp private data related to this instance.
	 */
    
    struct xen_shm_instance_data* instance_data = kmalloc(sizeof(struct xen_shm_instance_data), GFP_KERNEL /* sleeping is ok */);
    
    if (instance_data == NULL) {
        return -ENOMEM;
    }
    
    instance_data->state = XEN_SHM_STATE_OPENED;
    instance_data->local_domid = xen_shm_domid;
    
    filp->private_data = (void *) instance_data;
    
    return 0;

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
static int
xen_shm_release(struct inode * inode, struct file * filp)
{
    
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
    
    struct xen_shm_instance_data* data = (struct xen_shm_instance_data*) filp->private_data;
    
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
    if (data->state != XEN_SHM_STATE_OPENED) {
        free_pages(data->shared_memory, data->alloc_order); //TODO: Must only be done when other side ok
    }
    
    
    
    
    kfree(filp->private_data);  //TODO: Must only be done when other side ok

    return 0;    
}

/*
 * Used to map the shared pages into the user address space.
 */
static int
xen_shm_mmap(struct file *filp, struct vm_area_struct *vma)
{

	/*
	 * Test if the memory has been allocated
	 */

	/*
	 * Map the kernel's allocated memory into the user's space
	 */

    return 0;
}

static int
__xen_shm_ioctl_init_offerer(struct xen_shm_instance_data* data,
                             struct xen_shm_ioctlarg_offerer* arg)
{
    unsigned int order;
    uint32_t tmp_page_count;
    unsigned long alloc;
    
    if (data->state != XEN_SHM_STATE_OPENED) { /* Command is invalid in this state */
        return -ENOTTY;
    }
    
    if (arg->pages_count == 0 || arg->pages_count > XEN_SHM_MAX_SHARED_PAGES) { /* Cannot allocate this amount of pages */
        return -EINVAL;
    }
    
    /* 
     * Completing file data 
     */
    data->distant_domid = arg->dist_domid;
    
    
    /* 
     * Allocating memory 
     */
    
    //Computing the order of allocation size
    order = 0;
    tmp_page_count = arg->pages_count + 1;
    while (tmp_page_count != 0 ) {
        order++;
        tmp_page_count = tmp_page_count >> 1;
    }
    if (tmp_page_count==1<<(order-1)) {
        order--;
    }
    
    //Allocating the pages
    alloc = __get_free_pages(GFP_KERNEL, order);
    if (alloc == 0)
        return -ENOMEM;
    
    data->alloc_order = order;
    data->pages_count = tmp_page_count;
    data->shared_memory = alloc; 
    
    
    
    
    /* Initialize header page */
    //TODO
    
    /* Grant mapping and fill header page */
    //TODO
    
    /* Open event channel and connect it to handler */
    //TODO
    
    //TODO
    
    
    //If OK, the state is set to offerer
    data->state = XEN_SHM_STATE_OFFERER;   
    
    return 0;
}

static int
__xen_shm_ioctl_init_receiver(struct xen_shm_instance_data* data,
                              struct xen_shm_ioctlarg_receiver* arg)
{

    if (data->state != XEN_SHM_STATE_OPENED) { /* Command is invalid in this state */
        return -ENOTTY;
    }
    
    //TODO
    
    return 0;
}


/*
 * Used to control an open instance.
 */
static long
xen_shm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    
    /* The user pointer */
    void __user* arg_p = (void __user*) arg;
    
    /* The data related to this instance */
    struct xen_shm_instance_data* instance_data = (struct xen_shm_instance_data*) filp->private_data;
    
    /*
     * Structs for the different switchs
     */
    struct xen_shm_ioctlarg_offerer offerer_karg;
    struct xen_shm_ioctlarg_receiver receiver_karg;
    struct xen_shm_ioctlarg_getdomid getdomid_karg;
    
    /* retval */
    int retval = 0;
    
    /* Testing user's pointer */
    int err = 0; 
    
    /* Verifying value */
    if (_IOC_TYPE(cmd) != XEN_SHM_MAGIC_NUMBER) return -ENOTTY;
    //if (_IOC_NR(cmd) > XEN_SHM_IOCTL_MAXNR) return -ENOTTY; //Should be used when IOCTL MAXNR is defined
    
    /* Testing fault */
    if (_IOC_DIR(cmd) & _IOC_READ) //User wants to read, so kernel must write
        err = !access_ok(VERIFY_WRITE, arg_p, _IOC_SIZE(cmd));
    else if (_IOC_DIR(cmd) & _IOC_WRITE) //User wants to write, so kernel must read
        err = !access_ok(VERIFY_READ, arg_p, _IOC_SIZE(cmd));
    
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
           retval = copy_from_user(&offerer_karg, arg_p, sizeof(struct xen_shm_ioctlarg_offerer)); //Copying from userspace
           if (retval != 0)
               return -EFAULT;
               
           retval = __xen_shm_ioctl_init_offerer(instance_data, &offerer_karg);
           if (retval != 0)
               return retval;
           
           retval = copy_to_user(arg_p, &offerer_karg, sizeof(struct xen_shm_ioctlarg_offerer)); //Copying to userspace
           if (retval != 0)
               return -EFAULT;
           
           break;
       case XEN_SHM_IOCTL_INIT_RECEIVER:
           /*
            * Used to make the state go from OPENED to RECEIVER.
            * 
            */
           retval = copy_from_user(&receiver_karg, arg_p, sizeof(struct xen_shm_ioctlarg_receiver)); //Copying from userspace
           if (retval != 0)
               return -EFAULT;
           
           retval = __xen_shm_ioctl_init_receiver(instance_data, &receiver_karg);
           if (retval != 0)
               return retval;
           
           retval = copy_to_user(arg_p, &receiver_karg, sizeof(struct xen_shm_ioctlarg_receiver)); //Copying to userspace
           if (retval != 0)
               return -EFAULT;
           
           break;
       case XEN_SHM_IOCTL_WAIT:
           /*
            * Immediatly sends a signal through the signal channel
            */
           
           //TODO
           
           break;
       case XEN_SHM_IOCTL_SSIG:
           /*
            * Waits until a signal is received through the signal channel
            */
           
           //TODO
           
           break;
       case XEN_SHM_IOCTL_GET_DOMID:
           /*
            * Writes the domain id into the structure
            */
           getdomid_karg.local_domid = instance_data->local_domid; //Get the local_dom_id from the file data
           
           retval = copy_to_user(arg_p, &getdomid_karg, sizeof(struct xen_shm_ioctlarg_getdomid)); //Copying to userspace
           if (retval != 0)
               return -EFAULT;
           
           break;
       default:
           return -ENOTTY;
           break;

   }
    
    return 0;


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
