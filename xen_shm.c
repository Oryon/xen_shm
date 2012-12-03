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
#include <asm/page.h>
#include <asm/signal.h>
#include <asm/xen/hypercall.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <xen/interface/xen.h>
#include <xen/interface/event_channel.h>
#include <xen/events.h>
#include <xen/grant_table.h>
#include <xen/page.h>

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
 * Deal with moving functions
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 0, 0)
# define XEN_SHM_ALLOC_VM_AREA(x) alloc_vm_area(x)
#else /* LINUX_VERSION_CODE > KERNEL_VERSION(3, 0, 0) */
# define XEN_SHM_ALLOC_VM_AREA(x) alloc_vm_area(x, NULL)
#endif /* LINUX_VERSION_CODE ? KERNEL_VERSION(3, 0, 0) */

/*
 * Options : try to free delayed data on close/open ?
 */

#ifndef DELAYED_FREE_ON_CLOSE
# define DELAYED_FREE_ON_CLOSE 1  //Try to free delayed data when some descriptor is closed
#endif /* DELAYED_FREE_ON_CLOSE */

#ifndef DELAYED_FREE_ON_OPEN
# define DELAYED_FREE_ON_OPEN  1  //Try to free delayed data when some descriptor is opened
#endif /* DELAYED_FREE_ON_OPEN */

/*
 * The state of the shared memory
 */

enum xen_shm_state_t {
    XEN_SHM_STATE_OPENED,         /* Freshly opened device, can move to offerer or receiver */
    XEN_SHM_STATE_OFFERER,        /* Memory is allocated. Event pipe created. */
    XEN_SHM_STATE_RECEIVER,       /* Memory is allocated and only_first page mapped. Pipe is connected. */
    XEN_SHM_STATE_RECEIVER_MAPPED /* Memory is allocated and fully mapped. Pipe is connected. */
};

typedef uint8_t xen_shm_meta_page_state;

#define XEN_SHM_META_PAGE_STATE_NONE    0x01 //The peer didn't do anything
#define XEN_SHM_META_PAGE_STATE_OPENED  0x02 //The peer is using the pages
#define XEN_SHM_META_PAGE_STATE_CLOSED  0x03 //Receiver: Page is going to be unmapped -- Offerer: The receiver must close asap

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
static struct cdev xen_shm_cdev;
#define XEN_SHM_DEV_COUNT 1


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
    .mmap = xen_shm_mmap,
    .owner = THIS_MODULE,
};

/*
 * Defines the instance information.
 * They are stored in the private data field of 'struct file' and will maybe
 * have to be kept in order to "close later" in the case of a blocking offerer close.
 */
struct xen_shm_instance_data {
    enum xen_shm_state_t state; //The state of this instance


    /* Pages info */
    uint8_t pages_count;               //The total number of consecutive allocated pages (with the header page)
    unsigned long shared_memory;       //The kernel addresses of the allocated pages on the offerrer

    unsigned int offerer_alloc_order;  //Offerer only: Saved value of 'order'. Is used when freeing the pages

    struct vm_struct *unmapped_area;  //Receiver only: Virtual memeroy space allocated on the receiver
    unsigned long user_mapped_memory;  //Receiver only: Address where the user have mapped the shared memory


    /* Xen grant_table data */
    domid_t local_domid;    //The local domain id
    domid_t distant_domid;  //The distant domain id
    grant_ref_t first_page_grant;   //The first page grant reference

    grant_handle_t grant_map_handles[XEN_SHM_ALLOC_ALIGNED_PAGES]; //Receiver only: pages_count grant handles


    /* Event channel data */
    evtchn_port_t local_ec_port; //The allocated event port number
    evtchn_port_t dist_ec_port; //Receiver only: The allocated event port number

    /* Delayed memory next element */
    struct xen_shm_instance_data* next_delayed;

    /* Wait queue for the event channel */
    wait_queue_head_t wait_queue;
    unsigned int ec_irq;
    uint8_t wake_up;

};

/*
 * Delayed free queue
 */
static struct xen_shm_instance_data* xen_shm_delayed_free_queue = NULL;


/*
 * The first page is used to share meta-data in a more efficient way
 */
struct xen_shm_meta_page_data {

    xen_shm_meta_page_state offerer_state;
    xen_shm_meta_page_state receiver_state;

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



/*
 * Other private function prototypes
 */
static int __xen_shm_get_domid_hack(void);

//ioctl calls
static int __xen_shm_ioctl_init_offerer(struct xen_shm_instance_data* data, struct xen_shm_ioctlarg_offerer* arg);
static int __xen_shm_ioctl_init_receiver(struct xen_shm_instance_data* data, struct xen_shm_ioctlarg_receiver* arg);

//Shared memory
static void __xen_shm_free_shared_memory_receiver(struct xen_shm_instance_data* data);
static int __xen_shm_allocate_shared_memory_offerer(struct xen_shm_instance_data* data);
static void __xen_shm_free_shared_memory_offerer(struct xen_shm_instance_data* data);

//Event channel
static int __xen_shm_open_ec_offerer(struct xen_shm_instance_data* data);
static int __xen_shm_open_ec_receiver(struct xen_shm_instance_data* data);
static int __xen_shm_open_ec(struct xen_shm_instance_data* data, int offerer);

static int __xen_shm_close_ec_offerer(struct xen_shm_instance_data* data);
static int __xen_shm_close_ec_receiver(struct xen_shm_instance_data* data);
static int __xen_shm_close_ec(struct xen_shm_instance_data* data);

//Closure
static int __xen_shm_prepare_free(struct xen_shm_instance_data* data, bool first);
static void __xen_shm_add_delayed_free(struct xen_shm_instance_data* data);
#if (DELAYED_FREE_ON_CLOSE || DELAYED_FREE_ON_OPEN)
static void  __xen_shm_free_delayed_queue(void);
#endif /* (DELAYED_FREE_ON_CLOSE || DELAYED_FREE_ON_OPEN)*/

/*
 * Code :)
 */

static int
__xen_shm_get_domid_hack(void)
{
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
        return -EFBIG;
    }

    /*
     * Find domid if not given
     */
    if (xen_shm_domid == 0) {
         /* Let's try to get it by ouselves */
         res = __xen_shm_get_domid_hack();
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

    /*
     * Create cdev
     */
    cdev_init(&xen_shm_cdev, &xen_shm_file_ops);
    res = cdev_add(&xen_shm_cdev, xen_shm_device, XEN_SHM_DEV_COUNT);

    if (res < 0) {
        printk(KERN_WARNING "xen_shm: Unable to create cdev: %i\n", res);
        unregister_chrdev_region(xen_shm_device, 1);
        return res;
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
     * Try to free delayed closes (at least :'()
     */
    __xen_shm_free_delayed_queue();

    /*
     * Remove cdev
     */
    cdev_del(&xen_shm_cdev);

    /*
     * Unallocate the MAJOR number
     */
    unregister_chrdev_region(xen_shm_device, 1);

}

/*
 * Called when a user wants to open the device
 * Memory is not allocated yet because the size will be specified by the user with an ioctl.
 */
static int
xen_shm_open(struct inode * inode, struct file * filp)
{
    struct xen_shm_instance_data* instance_data;

    /*
     * Initialize the filp private data related to this instance.
     */
    instance_data = kmalloc(sizeof(struct xen_shm_instance_data), GFP_KERNEL /* sleeping is ok */);

    if (instance_data == NULL) {
        return -ENOMEM;
    }

    instance_data->state = XEN_SHM_STATE_OPENED;
    instance_data->local_domid = xen_shm_domid;
    instance_data->shared_memory = 0;
    instance_data->next_delayed = NULL;
    instance_data->unmapped_area = NULL;
    instance_data->wake_up = 0;

    filp->private_data = (void *) instance_data;

#if DELAYED_FREE_ON_OPEN
    //Try to close other delayed close
    __xen_shm_free_delayed_queue();
#endif /* DELAYED_FREE_ON_OPEN */

    /* Init the wait queue */
    init_waitqueue_head(&instance_data->wait_queue);

    return 0;
}

/*
 * After the users asked to close the file, it does everything to undo the mapping/granting.
 * Returns 0 if data can be safely forgot. A negative value if it cannot yet.
 */
static int
__xen_shm_prepare_free(struct xen_shm_instance_data* data, bool first)
{
    struct gnttab_unmap_grant_ref unmap_op;
    struct xen_shm_meta_page_data *meta_page_p;

    meta_page_p = (struct xen_shm_meta_page_data*) data->shared_memory;

    /*
     * Xen grant table state must be restored (unmap on receiver side and end grant on offerer side)
     */
    switch(data->state) {
        case XEN_SHM_STATE_OPENED:
            return 0;
        case XEN_SHM_STATE_OFFERER:
            //Try to free data pages first
            while (data->pages_count != 0) {
                if (gnttab_end_foreign_access_ref(meta_page_p->grant_refs[data->pages_count-1], 0)) {
                    data->pages_count--;
                } else {
                    //printk(KERN_WARNING "xen_shm: Grant ref %i (from first grant %i) still in use !\n", meta_page_p->grant_refs[data->pages_count-1], data->first_page_grant);
                    goto fail;
                }
            }

            //Closing event channel
            __xen_shm_close_ec_offerer(data);

            //Freeing memory
            __xen_shm_free_shared_memory_offerer(data);

            return 0;
        case XEN_SHM_STATE_RECEIVER_MAPPED:
            /* Try to unmap all user-mapped pages */
            while (data->pages_count > 1) {
                gnttab_set_unmap_op(&unmap_op, data->user_mapped_memory + PAGE_SIZE * (data->pages_count - 2), GNTMAP_host_map, data->grant_map_handles[data->pages_count - 1]);
                HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref, &unmap_op, 1);
                if (unmap_op.status == 0) {
                    data->pages_count--;
                } else {
                    if (first) {
                        printk(KERN_WARNING "xen_shm: Could not unmap a 'user'-mapped page in receiver mode. Error:%i !\n", unmap_op.status);
                    }
                    goto fail;
                }
            }
            // Continue ! (no break)
        case XEN_SHM_STATE_RECEIVER:
            // Unmap the first page
            gnttab_set_unmap_op(&unmap_op, ((unsigned long) data->shared_memory), GNTMAP_host_map, data->grant_map_handles[0]);
            HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref, &unmap_op, 1);
            if(unmap_op.status == 0) {
                data->pages_count--;
            } else {
                if (first) {
                    printk(KERN_WARNING "xen_shm: Could not unmap a page in receiver mode. Error:%i !\n", unmap_op.status);
                }
                goto fail;
            }

            //Closing event channel
            __xen_shm_close_ec_receiver(data);

            //Freeing memory
            __xen_shm_free_shared_memory_receiver(data);

            return 0;
        default:
            printk(KERN_WARNING "xen_shm: Impossible state !\n");
            return -2;
            break;
    }

fail:
    printk(KERN_WARNING "xen_shm: Failed to prepare free (first grant %i)\n", data->first_page_grant);
    return -1;
}

#if (DELAYED_FREE_ON_CLOSE || DELAYED_FREE_ON_OPEN)
static void
__xen_shm_free_delayed_queue(void)
{
    struct xen_shm_instance_data* current_i;
    struct xen_shm_instance_data* previous;
    struct xen_shm_instance_data* to_delete;

    previous = NULL;
    current_i = xen_shm_delayed_free_queue;

    while (current_i != NULL) {
        if (__xen_shm_prepare_free(current_i, false) == 0) { //On peut supprimer
            to_delete = current_i;
            if (previous == NULL) { //Premier élément de la liste
                xen_shm_delayed_free_queue = current_i->next_delayed;
            } else {
                previous->next_delayed = current_i->next_delayed;
            }
            current_i = current_i->next_delayed; //Next

            printk(KERN_WARNING "xen_shm: Finally freeing instance (%i) from the queue\n", to_delete->first_page_grant);
            kfree(to_delete);
        } else {
            previous = current_i;
            current_i = current_i->next_delayed;
        }
    }
}
#endif /* (DELAYED_FREE_ON_CLOSE || DELAYED_FREE_ON_OPEN) */

/*
 * Called when all the processes possessing this file descriptor closed it.
 * All the allocated memory must be deallocated.
 * Statefull data must be restored.
 */
static int
xen_shm_release(struct inode * inode, struct file * filp)
{
    struct xen_shm_instance_data* data;
    struct xen_shm_meta_page_data* meta_page_p;
    /*
     * Warning: Remember the OFFERER grants and ungrant the pages.
     *          The RECEIVER map and unmap the pages.
     *          BUT the OFFERER -must not- ungrant the pages before the RECEIVER unmaped them.
     *
     *          Xen functions to ungrant can be used when someone still map them, but the memory (obtained with kmalloc) cannot
     *          be freed because a new allocation of the same physical adresses would create troubles.
     *          void gnttab_end_foreign_access(grant_ref_t ref, int readonly, unsigned long page); maybe solve the problem ? --> Not implemented yet !?!!! (see sources)
     *            Maybe we should use the first page for information purposes ? (like using a value to know if the kmalloc can be freed)
     */

    data = (struct xen_shm_instance_data*) filp->private_data;
    
    
    switch (data->state) {
    case XEN_SHM_STATE_OPENED:

        break;
    case XEN_SHM_STATE_OFFERER:
        meta_page_p = (struct xen_shm_meta_page_data*) data->shared_memory;
        meta_page_p->offerer_state = XEN_SHM_META_PAGE_STATE_CLOSED;
        break;
    case XEN_SHM_STATE_RECEIVER:
    case XEN_SHM_STATE_RECEIVER_MAPPED:
        meta_page_p = (struct xen_shm_meta_page_data*) data->shared_memory;
        meta_page_p->receiver_state = XEN_SHM_META_PAGE_STATE_CLOSED;
        break;
    default:
        printk(KERN_WARNING "xen_shm: Impossibulu staytu !\n");
        return -2;
        break;
    }

    //Try to prepare the free
    if (__xen_shm_prepare_free(data, true)) {
        __xen_shm_add_delayed_free(data);
        return 0;
    }

    kfree(filp->private_data);

#if DELAYED_FREE_ON_CLOSE
    //Try to close other delayed close
    __xen_shm_free_delayed_queue();
#endif /* DELAYED_FREE_ON_CLOSE */

    return 0;
}


/*
 * Is called when a free cannot be done imidiatly. The data must be put in some queue and deleted later.
 */
static void
__xen_shm_add_delayed_free(struct xen_shm_instance_data* data)
{
    printk(KERN_WARNING "xen_shm: Data cannot be free. Adding to queue. !\n");
    data->next_delayed = xen_shm_delayed_free_queue;
    xen_shm_delayed_free_queue = data->next_delayed;
}

/*
 * Used to map the shared pages into the user address space.
 */
static int
xen_shm_mmap(struct file *filp, struct vm_area_struct *vma)
{
    int page, err;
    struct xen_shm_instance_data* data;
    struct xen_shm_meta_page_data* meta_page_p;

    struct gnttab_map_grant_ref map_op;
    struct gnttab_unmap_grant_ref unmap_op;
    unsigned long  mfn;
    char *page_pointer;
#ifdef DEBUG_OVERRIDE
    int page_tmp;
#endif /* DEBUG_OVERRIDE */

    data = (struct xen_shm_instance_data*) filp->private_data;

    switch(data->state) {
        case XEN_SHM_STATE_OPENED:
            // Too soon
            return -ENODATA;
        case XEN_SHM_STATE_OFFERER:
            // Verify size
            if (vma->vm_end - vma->vm_start != (data->pages_count - 1) * PAGE_SIZE) {
                printk(KERN_WARNING "xen_shm: Only mapping of the right size are accepted\n");
                return -EINVAL;
            }
            // Ok, map logical memory
            return remap_pfn_range(vma, vma->vm_start, virt_to_pfn(data->shared_memory + PAGE_SIZE), vma->vm_end - vma->vm_start, vma->vm_page_prot);
        case XEN_SHM_STATE_RECEIVER_MAPPED:
            // Too late
            return -EPIPE;
        case XEN_SHM_STATE_RECEIVER:
            // Verify size
            if (vma->vm_end - vma->vm_start != (data->pages_count - 1) * PAGE_SIZE) {
                printk(KERN_WARNING "xen_shm: Only mapping of the right size are accepted\n");
                return -EINVAL;
            }
            // Ok, map logical memory
            meta_page_p = (struct xen_shm_meta_page_data*) data->shared_memory;
            page_pointer = (char *)vma->vm_start;
            for (page = 1; page < data->pages_count; ++page) {
                gnttab_set_map_op(&map_op, (phys_addr_t) page_pointer , GNTMAP_host_map, meta_page_p->grant_refs[page], data->distant_domid);

                if (HYPERVISOR_grant_table_op(GNTTABOP_map_grant_ref, &map_op, 1)) {
                    printk(KERN_WARNING "xen_shm: HYPERVISOR map grant ref failed");
                    err = -EFAULT;
                    goto clean;
                }

                if (map_op.status < 0) {
                    printk(KERN_WARNING "xen_shm: HYPERVISOR map grant ref failed with err %i \n", map_op.status);
                    err = -EINVAL;
                    goto clean;
                }
                data->grant_map_handles[page] = map_op.handle;

                if (map_op.flags & GNTMAP_contains_pte) {
                    mfn = pte_mfn(*((pte_t *)(mfn_to_virt(PFN_DOWN(map_op.host_addr)) +(map_op.host_addr & ~PAGE_MASK))));
                } else {
                    mfn = PFN_DOWN(map_op.dev_bus_addr);
                }

                // The override doesn't work :'( skip it for now
#ifdef DEBUG_OVERRIDE
                // Overide whatever
                if (xen_feature(XENFEAT_auto_translated_physmap)) {
                    printk("xen_shm: Unable to enter lazy mode\n");
                }
                arch_enter_lazy_mmu_mode();

                err = m2p_add_override(mfn, virt_to_page(page_pointer), NULL);
                if (err != 0) {
                    printk(KERN_WARNING "xen_shm: Unable to add override page %d: %d\n", page - 1, err);
                    err = -EBADFD
                    goto clean_override;
                }
                arch_leave_lazy_mmu_mode();
#endif /* DEBUG_OVERRIDE */

                page_pointer += PAGE_SIZE;
            }
            data->user_mapped_memory = vma->vm_start;
            data->state = XEN_SHM_STATE_RECEIVER_MAPPED;
            return 0;
        default:
            printk(KERN_WARNING "xen_shm: Impossible state !\n");
            break;
    }

    return -ENOSYS;

clean:
#ifdef DEBUG_OVERRIDE
    if (xen_feature(XENFEAT_auto_translated_physmap)) {
        printk("xen_shm: Unable to enter lazy mode\n");
    }
    arch_enter_lazy_mmu_mode();
    for (page_tmp = page - 1; page_tmp > 0; --page_tmp) {
        m2p_remove_override(virt_to_page(vma->vm_start + (page_tmp * PAGE_SIZE)), 0);
    }
    arch_leave_lazy_mmu_mode();
#endif /* DEBUG_OVERRIDE */
    if (err != -EBADFD) {
      page--;
      page_pointer -= PAGE_SIZE;
    }
    for (; page > 0; --page) {
        gnttab_set_unmap_op(&unmap_op, (unsigned long) page_pointer, GNTMAP_host_map, data->grant_map_handles[page]);
        HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref, &unmap_op, 1);
    }

    return -EFAULT;
}

static irqreturn_t
xen_shm_event_handler(int irq, void* arg)
{
    struct xen_shm_instance_data* data;

    data = (struct xen_shm_instance_data*) arg;

    printk(KERN_WARNING "xen_shm: A signal has just been handled\n");
    data->wake_up = 1;
    wake_up_interruptible(&data->wait_queue);

    return IRQ_HANDLED; //Can also return IRQ_NONE or IRQ_WAKE_THREAD
}

static int
__xen_shm_open_ec_offerer(struct xen_shm_instance_data* data)
{
    return __xen_shm_open_ec(data, 1);
}


static int
__xen_shm_open_ec_receiver(struct xen_shm_instance_data* data)
{
    /* Read the distant port from the meta page */
    struct xen_shm_meta_page_data *meta_page_p;
    meta_page_p = (struct xen_shm_meta_page_data*) data->shared_memory;
    data->dist_ec_port = meta_page_p->offerer_ec_port;

    return __xen_shm_open_ec( data, 0);
}

static int
__xen_shm_open_ec(struct xen_shm_instance_data* data, int offerer)
{
    int retval;
    struct evtchn_alloc_unbound alloc_unbound;
    struct evtchn_bind_interdomain bind_op;
    struct evtchn_close close_op;

    if(offerer) { //Offerer case
        alloc_unbound.dom = DOMID_SELF;
        alloc_unbound.remote_dom = (data->distant_domid == data->local_domid)?DOMID_SELF:data->distant_domid;

        /* Open a unbound channel */
        if(HYPERVISOR_event_channel_op(EVTCHNOP_alloc_unbound, &alloc_unbound)) {
            return -EIO;
        }

        data->local_ec_port = alloc_unbound.port;

    } else { //Receiver case
        bind_op.remote_dom = data->distant_domid;
        bind_op.remote_port = data->dist_ec_port;

        if(HYPERVISOR_event_channel_op(EVTCHNOP_bind_interdomain, &bind_op)) {
            return -EIO;
        }

        data->local_ec_port = bind_op.local_port;
    }

    /* Set handler on unbound channel */
    retval = bind_evtchn_to_irqhandler(data->local_ec_port, xen_shm_event_handler, 0, "xen_shm", data);
    if(retval <= 0) {
        close_op.port = data->local_ec_port;
        if(HYPERVISOR_event_channel_op(EVTCHNOP_close, &close_op)) {
            printk(KERN_WARNING "xen_shm: Couldn't close event channel (state leak) \n", retval);
        }
        return -EIO;
    }
    data->ec_irq = retval;

    return 0;
}


static int
__xen_shm_close_ec_receiver(struct xen_shm_instance_data* data)
{
    return __xen_shm_close_ec(data);
}


static int
__xen_shm_close_ec_offerer(struct xen_shm_instance_data* data)
{
    return __xen_shm_close_ec(data);
}

static int
__xen_shm_close_ec(struct xen_shm_instance_data* data)
{
    unbind_from_irqhandler(data->ec_irq, data); //Also close the channel
    return 0;
}


static int
__xen_shm_allocate_shared_memory_offerer(struct xen_shm_instance_data* data)
{
    uint32_t tmp_page_count;
    unsigned int order;
    unsigned long alloc;

    // Computing the order of allocation size
    order = 0;
    tmp_page_count = data->pages_count;
    while (tmp_page_count != 0 ) {
        order++;
        tmp_page_count = tmp_page_count >> 1;
    }
    if (tmp_page_count==1<<(order-1)) {
        order--;
    }

    // Allocating the pages
    alloc = __get_free_pages(GFP_KERNEL, order);
    if (alloc == 0) {
        printk(KERN_WARNING "xen_shm: could not alloc space for 2^%i pages\n", (int) order);
        return -ENOMEM;
    }

    data->offerer_alloc_order = order;
    data->shared_memory = alloc;

    return 0;

}

//Free the offerer memory pages
static void
__xen_shm_free_shared_memory_offerer(struct xen_shm_instance_data* data)
{
    if (data->shared_memory != 0) {
        free_pages(data->shared_memory, data->offerer_alloc_order);
    }
}

//Free the receiver memory pages
static void
__xen_shm_free_shared_memory_receiver(struct xen_shm_instance_data* data)
{
    if (data->unmapped_area != NULL) {
      free_vm_area(data->unmapped_area);
    }
}



static int
__xen_shm_ioctl_init_offerer(struct xen_shm_instance_data* data,
                             struct xen_shm_ioctlarg_offerer* arg)
{
    int error;
    int page;
    char *page_pointer;
    struct xen_shm_meta_page_data *meta_page_p;

    if (data->state != XEN_SHM_STATE_OPENED) {
        /* Command is invalid in this state */
        return -ENOTTY;
    }

    if (arg->pages_count == 0 || arg->pages_count > XEN_SHM_MAX_SHARED_PAGES) {
        /* Cannot allocate this amount of pages */
        printk(KERN_WARNING "xen_shm: Pages count is out of bound (%i).",arg->pages_count );
        return -EINVAL;
    }

    /*
     * Completing file data
     */
    data->distant_domid = (arg->dist_domid == DOMID_SELF) ? data->local_domid : arg->dist_domid;
    data->pages_count = arg->pages_count + 1;

    /*
     * Allocating memory
     */
    error = __xen_shm_allocate_shared_memory_offerer(data);
    if (error < 0) {
        return error;
    }

    /* Initialize header page */
    meta_page_p = (struct xen_shm_meta_page_data*) data->shared_memory;
    meta_page_p->offerer_state = XEN_SHM_META_PAGE_STATE_NONE;
    meta_page_p->receiver_state = XEN_SHM_META_PAGE_STATE_NONE;

    meta_page_p->pages_count = data->pages_count;

    /* Grant mapping and fill header page */
    page_pointer = (char*) data->shared_memory;
    for (page=0; page < data->pages_count; page++) {
        meta_page_p->grant_refs[page] = gnttab_grant_foreign_access(data->distant_domid , virt_to_mfn(page_pointer), 0); //Granting access
        if (meta_page_p->grant_refs[page] < 0) { //In case of error
            error = meta_page_p->grant_refs[page];
            printk(KERN_WARNING "xen_shm: could not grant %ith page (%i)\n",page, (int) meta_page_p->grant_refs[page]);
            goto undo_grant;
        }

        page_pointer += PAGE_SIZE; //Go to next page
    }

    //Set argument respond
    arg->grant = meta_page_p->grant_refs[0];

    //Set first grant ref
    data->first_page_grant = meta_page_p->grant_refs[0];


    /* Open event channel and connect it to handler */
    if((error = __xen_shm_open_ec_offerer(data)) != 0) {
    	goto undo_grant;
    }


    /* If OK, states are changed*/
    data->state = XEN_SHM_STATE_OFFERER;
    meta_page_p->offerer_state = XEN_SHM_META_PAGE_STATE_OPENED;

    return 0;


undo_grant:
    page--;
    for (; page>=0; page--) {
        gnttab_end_foreign_access_ref(meta_page_p->grant_refs[page], 0);
    }
    __xen_shm_free_shared_memory_offerer(data);


    return error;
}

static int
__xen_shm_ioctl_init_receiver(struct xen_shm_instance_data* data,
                              struct xen_shm_ioctlarg_receiver* arg)
{
    int error;
    struct gnttab_map_grant_ref map_op;
    struct gnttab_unmap_grant_ref unmap_op;

    struct xen_shm_meta_page_data* meta_page_p;

    if (data->state != XEN_SHM_STATE_OPENED) {
        /* Command is invalid in this state */
        return -ENOTTY;
    }

    if (arg->pages_count == 0 || arg->pages_count > XEN_SHM_MAX_SHARED_PAGES) {
        /* Cannot allocate this amount of pages */
        printk(KERN_WARNING "xen_shm: Pages count is out of bound (%i).",arg->pages_count );
        return -EINVAL;
    }

    /*
     * Completing file data
     */
    data->distant_domid = (arg->dist_domid==DOMID_SELF)?data->local_domid:arg->dist_domid;
    data->first_page_grant = arg->grant;
    data->pages_count = arg->pages_count + 1;

    /*
     * Allocating memory space
     */
    data->unmapped_area = XEN_SHM_ALLOC_VM_AREA(PAGE_SIZE);
    if (data->unmapped_area == NULL) {
        printk(KERN_WARNING "xen_shm: Cannot allocate vm area.");
        return -ENOMEM;
    }
    data->shared_memory = (unsigned long) data->unmapped_area->addr;

    /*
     * Finding the first page
     */
    gnttab_set_map_op(&map_op, (phys_addr_t) data->unmapped_area->addr, GNTMAP_host_map, data->first_page_grant, data->distant_domid);

    if (HYPERVISOR_grant_table_op(GNTTABOP_map_grant_ref, &map_op, 1)) {
        printk(KERN_WARNING "xen_shm: HYPERVISOR map grant ref failed\n");
        error = -EFAULT;
        goto undo_alloc;
    }

    if (map_op.status < 0) {
        printk(KERN_WARNING "xen_shm: HYPERVISOR map grant ref failed with error %i \n", map_op.status);
        error = -EINVAL;
        goto undo_alloc;
    }

    data->grant_map_handles[0] = map_op.handle;

    /*
     * Checking compatibility
     */
    meta_page_p = (struct xen_shm_meta_page_data*) data->shared_memory;

    if (data->pages_count != meta_page_p->pages_count ) { //Not the same number of pages on both sides
        printk(KERN_WARNING "xen_shm: Pages count is incorrect. Locally %i VS %i distantly.\n", (int)data->pages_count, (int)meta_page_p->pages_count );
        error = -EINVAL;
        goto undo_map;
    }

    /*
     * The rest will be mapped on 'mmap' command
     */

    /*
     * Connecting the event channel
     */
    /* Open event channel and connect it to handler */
    if((error = __xen_shm_open_ec_receiver(data)) != 0) {
        goto undo_map;
    }



    /* If OK, states are changed*/
    data->state = XEN_SHM_STATE_RECEIVER;
    meta_page_p->receiver_state = XEN_SHM_META_PAGE_STATE_OPENED;

    return 0;

undo_map:
    gnttab_set_unmap_op(&unmap_op, (unsigned long) data->unmapped_area->addr, GNTMAP_host_map, data->grant_map_handles[0]);
    HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref, &unmap_op, 1);

undo_alloc:
    __xen_shm_free_shared_memory_receiver(data);

    return error;
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
             * Waits until a signal is received through the signal channel
             */
            if(instance_data->state == XEN_SHM_STATE_OFFERER || instance_data->state == XEN_SHM_STATE_RECEIVER) {
                instance_data->wake_up = 0; //Prepare the condition
                retval = wait_event_interruptible(instance_data->wait_queue, instance_data->wake_up );
                if (retval != 0) {
                    return retval;
                }
            } else {
                /* Command is invalid in this state */
                return -ENOTTY;
            }

            break;
        case XEN_SHM_IOCTL_SSIG:
            /*
             * Immediatly sends a signal through the signal channel
             */

            if(instance_data->state == XEN_SHM_STATE_OFFERER || instance_data->state == XEN_SHM_STATE_RECEIVER) {
                notify_remote_via_evtchn(instance_data->local_ec_port);
            } else {
                /* Command is invalid in this state */
                return -ENOTTY;
            }

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
