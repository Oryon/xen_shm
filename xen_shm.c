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

#define DEBUG 1

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
#include <linux/mmu_notifier.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <xen/balloon.h>
#include <xen/interface/xen.h>
#include <xen/interface/event_channel.h>
#include <xen/events.h>
#include <xen/grant_table.h>
#include <xen/page.h>

/*
 * The public header of this module
 */
#include "xen_shm.h"


/*
 * Deal with old linux/xen
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 0, 0)
# define XEN_SHM_ALLOC_VM_AREA(x) alloc_vm_area(x)
# define gnttab_map_refs(map_ops, x, y, count)                            \
    HYPERVISOR_grant_table_op(GNTTABOP_map_grant_ref, map_ops, count)
# define gnttab_unmap_refs(unmap_ops, x, count, y)                        \
    HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref, unmap_ops, count)
#else /* LINUX_VERSION_CODE > KERNEL_VERSION(3, 0, 0) */
# define XEN_SHM_ALLOC_VM_AREA(x) alloc_vm_area(x, NULL)
#endif /* LINUX_VERSION_CODE ? KERNEL_VERSION(3, 0, 0) */


/*
 * Fix USHRT_MAX declaration on strange linux
 */
#ifndef USHRT_MAX
# define USHRT_MAX  ((u16)(~0U))
#endif


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
 * Private Types & Structures
 */

/* Internal module state */
enum xen_shm_state_t {
    XEN_SHM_STATE_OPENED,         /* Freshly opened device, can move to offerer or receiver */
    XEN_SHM_STATE_OFFERER,        /* Memory is allocated. Event pipe created. */
    XEN_SHM_STATE_RECEIVER,       /* Memory is allocated and only_first page mapped. Pipe is connected. */
    XEN_SHM_STATE_RECEIVER_MAPPED /* Memory is allocated and fully mapped. Pipe is connected. */
};

/* Page state */
typedef uint8_t xen_shm_meta_page_state;
#define XEN_SHM_META_PAGE_STATE_NONE    0x01 //The peer didn't do anything
#define XEN_SHM_META_PAGE_STATE_OPENED  0x02 //The peer is using the pages
#define XEN_SHM_META_PAGE_STATE_CLOSED  0x03 //Receiver: Page is going to be unmapped -- Offerer: The receiver must close asap


/*
 * Defines the instance information.
 * They are stored in the private data field of 'struct file' and will maybe
 * have to be kept in order to "close later" in the case of a blocking offerer close.
 */
struct xen_shm_instance_data {
    enum xen_shm_state_t state; //The state of this instance

    /* Use PTE or nor ?*/
    int use_ptemod;

    /* Pages info */
    uint8_t pages_count;               //The total number of consecutive allocated pages (with the header page)
    unsigned long shared_memory;       //The kernel addresses of the allocated pages on the offerrer

    /* Xen domids */
    domid_t local_domid;    //The local domain id
    domid_t distant_domid;  //The distant domain id


    /* Event channel data */
    evtchn_port_t local_ec_port; //The allocated event port number
    evtchn_port_t dist_ec_port; //Receiver only: The allocated event port number

    /* Delayed memory next element */
    struct xen_shm_instance_data* next_delayed;

    /* Wait queue for the event channel */
    wait_queue_head_t wait_queue;
    unsigned int ec_irq;
    uint8_t initial_signal;
    uint8_t user_signal;

    /* State depend variables */
    /* Both */
    grant_ref_t first_page_grant;   //The first page grant reference

    /* Offerrer only */
    unsigned int offerer_alloc_order;  //Offerer only: Saved value of 'order'. Is used when freeing the pages

    /* Receiver only */
    struct vm_struct *unmapped_area;  //Receiver only: Virtual memeroy space allocated on the receiver
    struct vm_area_struct *user_mem;  //Receiver only: Address where the user have mapped the shared memory
    struct page *user_pages[XEN_SHM_ALLOC_ALIGNED_PAGES];

    grant_handle_t         grant_map_handles[XEN_SHM_ALLOC_ALIGNED_PAGES]; //Receiver only: pages_count grant handles
    struct gnttab_map_grant_ref      map_ops[XEN_SHM_ALLOC_ALIGNED_PAGES];
    struct gnttab_unmap_grant_ref  unmap_ops[XEN_SHM_ALLOC_ALIGNED_PAGES];
    struct gnttab_map_grant_ref     kmap_ops[XEN_SHM_ALLOC_ALIGNED_PAGES];

    struct mm_struct *mm;
    struct mmu_notifier mn;
};

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
 * Global values
 */
#define XEN_SHM_DEV_COUNT 1
static domid_t xen_shm_domid = 0; //Must only be used in open Use instance_data to get it otherwise.
static int xen_shm_major_number = XEN_SHM_MAJOR_NUMBER;
static int xen_shm_minor_number = 0;
static dev_t xen_shm_device = 0;
static struct cdev xen_shm_cdev;
static struct xen_shm_instance_data* xen_shm_delayed_free_queue = NULL;

/*
 * Module parameters
 * As we don't want to define the checkers for domid_t, let's say it's a ushort
 */
module_param(xen_shm_domid, ushort, S_IRUSR | S_IRGRP);
MODULE_PARM_DESC(xen_shm_domid, "Local domain id");


/**********************************************************************************/


/*****************
 * Event Channel *
 *****************/


/*
 * Signal handler
 */
static irqreturn_t
xen_shm_event_handler(int irq, void* arg)
{
    struct xen_shm_instance_data* data;

    data = (struct xen_shm_instance_data*) arg;

    printk(KERN_WARNING "xen_shm: A signal has just been handled\n");
    if(data->initial_signal) {
        if(data->state == XEN_SHM_STATE_OFFERER) { //Responds to the initial signal
            notify_remote_via_evtchn(data->local_ec_port);
        }
        data->initial_signal = 1;
    } else {
        data->user_signal = 1;
    }

    wake_up_interruptible(&data->wait_queue);

    return IRQ_HANDLED; //Can also return IRQ_NONE or IRQ_WAKE_THREAD
}


/*
 * Generic canal openning
 */
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
            printk(KERN_WARNING "xen_shm: Couldn't close event channel (state leak) \n");
        }
        return -EIO;
    }
    data->ec_irq = retval;

    return 0;
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


/*
 * Generic canal closing
 */
static int
__xen_shm_close_ec(struct xen_shm_instance_data* data)
{
    unbind_from_irqhandler(data->ec_irq, data); //Also close the channel
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



/**********************************************************************************/


/******************************
 * User mapped memory helpers *
 ******************************/

static int
__xen_shm_contruct_receiver_k_ops(pte_t *pte, pgtable_t token, unsigned long addr, void *inc)
{
    struct xen_shm_instance_data *data;
    struct xen_shm_meta_page_data* meta_page_p;
    unsigned int offset;
    u64 pte_maddr;

    data = (struct xen_shm_instance_data*) inc;
    offset = (addr - data->user_mem->vm_start) >> PAGE_SHIFT;
    pte_maddr = arbitrary_virt_to_machine(pte).maddr;
    meta_page_p = (struct xen_shm_meta_page_data*) data->shared_memory;

    gnttab_set_map_op(data->map_ops + offset, pte_maddr,
                      GNTMAP_host_map | GNTMAP_application_map | GNTMAP_contains_pte,
                      meta_page_p->grant_refs[offset + 1], data->distant_domid);
    gnttab_set_unmap_op(data->unmap_ops + offset, pte_maddr,
                      GNTMAP_host_map | GNTMAP_application_map | GNTMAP_contains_pte,
                      -1 /* Non valid handler */);
    return 0;
}


static void
__xen_shm_unmap_receiver_grant_pages(struct xen_shm_instance_data *data, int offset, int nb)
{
    while(nb > 0) {
        if (data->unmap_ops[offset].handle != -1) {
            if (gnttab_unmap_refs(data->unmap_ops + offset, data->user_pages + offset, 1, true)) {
                printk(KERN_WARNING "xen_shm: error while unmapping refs\n");
            }
            --nb;
        }
        ++offset;
        if (offset >= data->pages_count - 1) {
            if (nb != 0) {
                printk(KERN_WARNING "xen_shm: Still %i pages to unmap but no more space\n", nb);
            }
            return;
        }
    }
}



/**********************************************************************************/


/*********************
 * Memory management *
 *********************/


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
            if (!data->use_ptemod) {
                 __xen_shm_unmap_receiver_grant_pages(data, 0, data->pages_count - 1);
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



/**********************************************************************************/


/***************************
 * mmu_notifier operations *
 ***************************/

static void
__xen_shm_mn_invl_range_start(struct mmu_notifier *mn,
                              struct mm_struct *mm,
                              unsigned long start, unsigned long end)
{
    struct xen_shm_instance_data *data;
    unsigned long mstart, mend;

    data = container_of(mn, struct xen_shm_instance_data, mn);

    if (data->user_mem == NULL) {
        /* Wrong state no need to do anything */
        return;
    }
    if (start >= data->user_mem->vm_end || data->user_mem->vm_start >= end) {
        /* Not sthe right area */
        return;
    }
    mstart = max(start, data->user_mem->vm_start);
    mend   = min(end,   data->user_mem->vm_end);
    __xen_shm_unmap_receiver_grant_pages(data, (mstart - data->user_mem->vm_start) >> PAGE_SHIFT, (mend - mstart) >> PAGE_SHIFT);
}


static void
__xen_shm_mn_invl_page(struct mmu_notifier *mn,
                       struct mm_struct *mm,
                       unsigned long address)
{
        __xen_shm_mn_invl_range_start(mn, mm, address, address + PAGE_SIZE);
}


static void
__xen_shm_mn_release(struct mmu_notifier *mn,
                     struct mm_struct *mm)
{
    struct xen_shm_instance_data *data;

    data = container_of(mn, struct xen_shm_instance_data, mn);
    __xen_shm_unmap_receiver_grant_pages(data, 0, data->pages_count - 1);
}


struct mmu_notifier_ops xen_shm_mmu_ops = {
        .invalidate_range_start = __xen_shm_mn_invl_range_start,
        .invalidate_page        = __xen_shm_mn_invl_page,
        .release                = __xen_shm_mn_release,
};



/**********************************************************************************/


/******************
 * IOCTLs helpers *
 ******************/


/*
 * Helper for XEN_SHM_IOCTL_INIT_OFFERER
 */
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
    meta_page_p->offerer_ec_port = data->local_ec_port;


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


/*
 * Helper for XEN_SHM_IOCTL_INIT_RECEIVER
 */
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

    /* Send the initial signal */
    notify_remote_via_evtchn(data->local_ec_port);



    return 0;

undo_map:
    gnttab_set_unmap_op(&unmap_op, (unsigned long) data->unmapped_area->addr, GNTMAP_host_map, data->grant_map_handles[0]);
    HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref, &unmap_op, 1);

undo_alloc:
    __xen_shm_free_shared_memory_receiver(data);

    return error;
}


/*
 * Detect broken pipes
 */
static int
__xen_shm_is_broken_pipe(struct xen_shm_meta_page_data* meta_page_p) {
    return (meta_page_p->offerer_state == XEN_SHM_META_PAGE_STATE_CLOSED
            || meta_page_p->receiver_state == XEN_SHM_META_PAGE_STATE_CLOSED);
}


/*
 * Helper for XEN_SHM_IOCTL_WAIT and XEN_SHM_IOCTL_AWAIT
 */
static int
__xen_shm_ioctl_await(struct xen_shm_instance_data* data,
                      struct xen_shm_ioctlarg_await* arg)
{
    struct xen_shm_meta_page_data* meta_page_p;
    unsigned long jiffies;
    int retval;
    int user_flag;
    int init_flag;

    user_flag = arg->request_flags & XEN_SHM_IOCTL_AWAIT_USER;
    init_flag = arg->request_flags & XEN_SHM_IOCTL_AWAIT_INIT;

    if(data->state == XEN_SHM_STATE_OPENED) //Not opened yet
    {
        return -ENOTTY;
    }

    meta_page_p = (struct xen_shm_meta_page_data*) data->shared_memory;

    //Condition telling wether the pipe is known to be closed

    data->user_signal = 0; //Trigger the wait

    if(arg->timeout_ms == 0) {
        retval = wait_event_interruptible(data->wait_queue,
                (user_flag&&data->user_signal) || (init_flag&&data->initial_signal) ||
                __xen_shm_is_broken_pipe(meta_page_p));
        if(retval < 0) {
            return retval;
        }
    } else {
        jiffies = (arg->timeout_ms*HZ)/1000;
        retval = wait_event_interruptible_timeout(data->wait_queue,
                (user_flag&&data->user_signal) || (init_flag&&data->initial_signal) ||
                __xen_shm_is_broken_pipe(meta_page_p),
                        jiffies);
        if(retval < 0) {
            return retval;
        }
        arg->remaining_ms = (retval==jiffies)?arg->timeout_ms:(retval*1000)/HZ;
        retval = 0;
    }

    if(__xen_shm_is_broken_pipe(meta_page_p)) {
        return -EPIPE;
    }
#undef XEN_SHM_IOCTL_AWAIT_COND
    return 0;

}


/*
 * Helper for XEN_SHM_IOCTL_SSIG
 */
static int
__xen_shm_ioctl_ssig(struct xen_shm_instance_data* data) {

    if(data->state == XEN_SHM_STATE_OPENED) {
        return -ENOTTY;
    }

    if(__xen_shm_is_broken_pipe((struct xen_shm_meta_page_data*) data->shared_memory)) {
        return -EPIPE;
    }

    notify_remote_via_evtchn(data->local_ec_port);

    return 0;
}



/**********************************************************************************/


/*******************
 * file operations *
 *******************/


/*
 * OPEN.
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
    instance_data->user_mem = NULL;
    instance_data->initial_signal = 0;
    instance_data->user_signal = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 0, 0)
    instance_data->use_ptemod = 0;
#else /* LINUX_VERSION_CODE > KERNEL_VERSION(3, 0, 0) */
    instance_data->use_ptemod = xen_pv_domain();
    if (instance_data->use_ptemod) {
        instance_data->mm = get_task_mm(current);
        if (instance_data->mm == NULL) {
            goto clean;
        }
        instance_data->mn.ops = &xen_shm_mmu_ops;
        if (mmu_notifier_register(&instance_data->mn, instance_data->mm) != 0) {
            goto clean;
        }
        mmput(instance_data->mm);
    }
#endif /* LINUX_VERSION_CODE ?KERNEL_VERSION(3, 0, 0) */

    filp->private_data = (void *) instance_data;

#if DELAYED_FREE_ON_OPEN
    //Try to close other delayed close
    __xen_shm_free_delayed_queue();
#endif /* DELAYED_FREE_ON_OPEN */

    /* Init the wait queue */
    init_waitqueue_head(&instance_data->wait_queue);

    return 0;

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 0, 0)
clean:
    kfree(instance_data);
    return -ENOMEM;
#endif /* LINUX_VERSION_CODE > KERNEL_VERSION(3, 0, 0) */
}


/*
 * IOCTL.
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
    struct xen_shm_ioctlarg_await await_karg;

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
            await_karg.remaining_ms = 0;
            await_karg.request_flags = XEN_SHM_IOCTL_AWAIT_INIT | XEN_SHM_IOCTL_AWAIT_USER;

            return __xen_shm_ioctl_await(instance_data, &await_karg);

            break;
        case XEN_SHM_IOCTL_AWAIT:
            retval = copy_from_user(&await_karg, arg_p, sizeof(struct xen_shm_ioctlarg_await)); //Copying from userspace
            if (retval != 0)
                return -EFAULT;

            retval = __xen_shm_ioctl_await(instance_data, &await_karg);
            if (retval != 0)
                return retval;

            retval = copy_to_user(arg_p, &await_karg, sizeof(struct xen_shm_ioctlarg_await)); //Copying to userspace
            if (retval != 0)
                return -EFAULT;

            break;
        case XEN_SHM_IOCTL_SSIG:
            /*
             * Immediatly sends a signal through the signal channel
             */

            return __xen_shm_ioctl_ssig(instance_data);

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
 * MMAP.
 * Used to map the shared pages into the user address space.
 */
static int
xen_shm_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct xen_shm_instance_data* data;
    struct xen_shm_meta_page_data* meta_page_p;
    int offset, err;
    unsigned dummy;
    phys_addr_t addr;

    if ((vma->vm_flags & VM_WRITE) && !(vma->vm_flags & VM_SHARED)) {
        return -EINVAL;
    }

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
            if ((vma->vm_end - vma->vm_start) >> PAGE_SHIFT != data->pages_count - 1) {
                printk(KERN_WARNING "xen_shm: Only mapping of the right size are accepted\n");
                return -EINVAL;
            }

            /* Setting the correct flags */
            vma->vm_flags |= VM_RESERVED|VM_DONTEXPAND;
            if (data->use_ptemod) {
                vma->vm_flags |= VM_DONTCOPY;
            }
            // Allocate pages
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 0, 0)
            for(offset = 0; offset < data->pages_count - 1; ++offset) {
                data->user_pages[offset] = alloc_page(GFP_KERNEL | __GFP_HIGHMEM);
                if (data->user_pages == NULL) {
                    printk(KERN_WARNING "xen_shm: Unable to get enough pages\n");
                    goto clean_pages;
                }
            }
#else
            err = alloc_xenballooned_pages(data->pages_count - 1, data->user_pages, false);
            if (err != 0) {
                printk(KERN_WARNING "xen_shm: Unable to get xenballooned_pages : %i\n", err);
                goto clean_pages;
            }
#endif
            /* Store the vm_area_struct for latter use */
            data->user_mem = vma;
            /* Create map ops */
            meta_page_p = (struct xen_shm_meta_page_data*) data->shared_memory;
            if (data->use_ptemod) {
                err = apply_to_page_range(vma->vm_mm, vma->vm_start, vma->vm_end - vma->vm_start, __xen_shm_contruct_receiver_k_ops, data);
                if (err != 0) {
                    printk(KERN_WARNING " apply_to_page_range __xen_shm_contruct_receiver_k_ops faile : %i\n", err);
                    goto clean_pages;
                }
                for(offset = 0; offset < data->pages_count - 1; ++offset) {
                    addr = (phys_addr_t) pfn_to_kaddr(page_to_pfn(data->user_pages[offset]));
                    addr = arbitrary_virt_to_machine(lookup_address(addr, &dummy)).maddr;
                    gnttab_set_map_op(data->kmap_ops + offset, addr, GNTMAP_host_map | GNTMAP_contains_pte, meta_page_p->grant_refs[offset + 1], data->distant_domid);
                }
            } else {
                for(offset = 0; offset < data->pages_count - 1; ++offset) {
                    addr = (phys_addr_t) pfn_to_kaddr(page_to_pfn(data->user_pages[offset]));
                    gnttab_set_map_op(data->map_ops + offset, addr, GNTMAP_host_map, meta_page_p->grant_refs[offset + 1], data->distant_domid);
                    gnttab_set_unmap_op(data->unmap_ops + offset, GNTMAP_host_map, meta_page_p->grant_refs[offset + 1], -1/* Non valid handler */);
                }
            }
            /* Map everything ! */
            err = gnttab_map_refs(data->map_ops, data->use_ptemod ? data->kmap_ops : NULL, data->user_pages, data->pages_count - 1);
            /* Check */
            if (err != 0) {
                printk(KERN_WARNING "xen_shm: Unable to grant ref (err  %i)\n", err);
                err = -EFAULT;
                goto clean;
            }
            for (offset = 0; offset < data->pages_count - 1; ++offset) {
                if (data->map_ops[offset].status != 0) {
                    err = -EINVAL;
                } else {
                    data->unmap_ops[offset].handle = data->map_ops[offset].handle;
                }
            }
            if (err != 0) {
                printk(KERN_WARNING "xen_shm: Some grant failed !\n");
                err = -EFAULT;
                goto clean;
            }
            if (!data->use_ptemod) {
                for (offset = 0; offset < data->pages_count - 1; ++offset) {
                    err = vm_insert_page(vma, vma->vm_start + (offset * PAGE_SIZE), data->user_pages[offset]);
                    if (err != 0) {
                        printk(KERN_WARNING "xen_shm: vm_insert_page failed: %i\n", err);
                        goto clean;
                    }
                }
            }
            /* State change ! */
            data->state = XEN_SHM_STATE_RECEIVER_MAPPED;
            return 0;
        default:
            printk(KERN_WARNING "xen_shm: Impossible state !\n");
            break;
    }

    return -ENOSYS;


clean:
    for (offset = 0; offset < data->pages_count - 1; ++offset) {
        if (data->unmap_ops[offset].handle != -1) {
            if (gnttab_unmap_refs(data->unmap_ops + offset, data->user_pages + offset, 1, true)) {
                printk(KERN_WARNING "xen_shm: error while unmapping refs\n");
            }
        }
    }
clean_pages:
    data->user_mem = NULL;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 0, 0)
    for(offset = 0; offset < data->pages_count - 1; ++offset) {
        if (data->user_pages == NULL) {
            return -EFAULT;
        } else {
            __free_page(data->user_pages[offset]);
        }
    }
#else
    free_xenballooned_pages(data->pages_count - 1, data->user_pages);
#endif
    return -EFAULT;
}


/*
 * RELEASE.
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

        notify_remote_via_evtchn(data->local_ec_port); //Sends a signal to wake up waiting processes
        wake_up_interruptible(&data->wait_queue); //Wake up local waiting processes

        break;
    case XEN_SHM_STATE_RECEIVER:
    case XEN_SHM_STATE_RECEIVER_MAPPED:
        meta_page_p = (struct xen_shm_meta_page_data*) data->shared_memory;
        meta_page_p->receiver_state = XEN_SHM_META_PAGE_STATE_CLOSED;

        notify_remote_via_evtchn(data->local_ec_port); //Sends a signal to wake up waiting processes
        wake_up_interruptible(&data->wait_queue); //Wake up local waiting processes

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

    if (data->use_ptemod) {
        mmu_notifier_unregister(&data->mn, data->mm);
    }

    return 0;
}


/*
 * Defines the device file operations
 */
const struct file_operations xen_shm_file_ops = {
    .owner = THIS_MODULE,
    .open = xen_shm_open,
    .unlocked_ioctl = xen_shm_ioctl,
    .mmap = xen_shm_mmap,
    .release = xen_shm_release,
};



/**********************************************************************************/


/******************
 * Module  helper *
 ******************/

/*
 * Use a unbound event channel to learn or domid
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



/**********************************************************************************/


/****************
 * Module Level *
 ****************/

/*
 * Called when the module is loaded into the kernel
 */
int __init
xen_shm_init(void)
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
xen_shm_cleanup(void)
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
 * Declaring module functions
 */

module_init(xen_shm_init);
module_exit(xen_shm_cleanup);


/*
 * Module informations
 */

MODULE_LICENSE(MOD_LICENSE);
MODULE_AUTHOR(MOD_AUTHORS);
MODULE_DESCRIPTION(MOD_DESC);
